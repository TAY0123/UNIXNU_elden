/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <kern/kalloc.h>
#include <vm/memory_types.h>
#include "AdrenoFirmware.c"
#include "EldenAdreno830.c"
#include "EldenAdreno830GmuImage.h"
#include "Sm8750CmdDb.c"

static uint32_t
elden_adreno830_fnv1a(const void *data, size_t bytes)
{
    const uint8_t *p = data;
    uint32_t digest = 2166136261u;
    for (size_t i = 0; i < bytes; ++i) {
        digest = (digest ^ p[i]) * 16777619u;
    }
    return digest;
}

#ifdef ELDEN_ADRENO830_HFI_PROBE
#include "EldenAdreno830HfiBoot.inc"
#endif

static uint32_t *
elden_adreno830_copy_cmd_db(void)
{
    uint32_t *snapshot = kalloc_data(Sm8750CmdDbSize, Z_WAITOK);
    if (!snapshot) {
        printf("ELDEN-A830: command database allocation failed\n");
        return NULL;
    }
    vm_offset_t mapping = io_map(Sm8750CmdDbPhysicalBase, Sm8750CmdDbSize, VM_WIMG_IO,
                                 VM_PROT_READ, true);
    if (!mapping) {
        kfree_data(snapshot, Sm8750CmdDbSize);
        printf("ELDEN-A830: command database map failed\n");
        return NULL;
    }
    const volatile uint32_t *db = (const volatile uint32_t *)mapping;
    if (db[1] != Sm8750CmdDbMagic) {
        ml_io_unmap(mapping, Sm8750CmdDbSize);
        kfree_data(snapshot, Sm8750CmdDbSize);
        printf("ELDEN-A830: command database magic mismatch\n");
        return NULL;
    }
    for (size_t i = 0; i < Sm8750CmdDbSize / sizeof(uint32_t); ++i) {
        snapshot[i] = db[i];
    }
    ml_io_unmap(mapping, Sm8750CmdDbSize);
    return snapshot;
}

#ifdef ELDEN_RECOVERY_RPMH_READ
#include <kern/clock.h>
#include "Sm8750RpmhRead.c"

static uint32_t
elden_adreno830_rpmh_read32(void *context, uint32_t offset)
{
    uint32_t value = ((const volatile uint32_t *)context)[offset / 4];
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

static void
elden_adreno830_rpmh_write32(void *context, uint32_t offset, uint32_t value)
{
    volatile uint32_t *reg = (volatile uint32_t *)context + offset / 4;
    __asm__ volatile("str %w0, [%1]\n\tdsb sy" :: "r"(value), "r"(reg) : "memory");
}

static void
elden_adreno830_rpmh_delay(void *context __unused, unsigned us)
{
    delay((int)us);
}

#include "EldenAdreno830PowerProbe.inc"

static void
elden_adreno830_rpmh_triggering(void *context __unused)
{
    printf("ELDEN-A830: RPMh triggering DRV-2 TCS1 MX read\n");
}

static void
elden_adreno830_rpmh_vote_triggering(void *context __unused)
{
    printf("ELDEN-A830: RPMh triggering DRV-2 TCS1 rail vote\n");
}

void elden_adreno830_run_rpmh_read(unsigned stage);

void
elden_adreno830_run_rpmh_read(unsigned stage)
{
    uint32_t *snapshot = elden_adreno830_copy_cmd_db();
    if (!snapshot) {
        return;
    }
    Sm8750CmdDbResource mx;
    Sm8750CmdDbStatus found = Sm8750CmdDbRead(snapshot, Sm8750CmdDbSize, "mx.lvl", Sm8750CmdDbArc, &mx);
    if (found != Sm8750CmdDbOk || mx.address != 0x30010) {
        printf("ELDEN-A830: RPMh MX resource differs from the Elden capture\n");
        kfree_data(snapshot, Sm8750CmdDbSize);
        return;
    }
    printf("ELDEN-A830: RPMh mapping DRV-2 for one MX read\n");
    vm_offset_t mapping = io_map(SM8750_APPS_RSC_DRV2_BASE, SM8750_APPS_RSC_DRV2_PROBE_SIZE,
                                 VM_WIMG_IO, VM_PROT_READ | VM_PROT_WRITE, true);
    if (!mapping) {
        printf("ELDEN-A830: RPMh DRV-2 writable map failed\n");
        kfree_data(snapshot, Sm8750CmdDbSize);
        return;
    }
    Sm8750RpmhReadIo io = {
        (void *)mapping, elden_adreno830_rpmh_read32, elden_adreno830_rpmh_write32,
        elden_adreno830_rpmh_delay, elden_adreno830_rpmh_triggering,
    };
    Sm8750RpmhReadResult result;
    Sm8750RpmhReadStatus status = Sm8750RpmhReadArc(&io, mx.address, &result);
    printf("ELDEN-A830: RPMh MX read result=%u id=%08x config=%08x reg=%04x irq=%08x cmd=%08x response=%08x\n",
           (unsigned)status, result.hardware, result.config, result.failed_register,
           result.irq_status, result.command_status, result.response);
    Sm8750RpmhGpuRailResult rails = {0};
    if (stage >= 2 && status == Sm8750RpmhReadOk) {
        io.triggering = elden_adreno830_rpmh_vote_triggering;
        status = Sm8750RpmhHoldGpuRails(&io, snapshot, Sm8750CmdDbSize, &rails);
        printf("ELDEN-A830: RPMh rail hold result=%u MX=%u trigger=%u ack=%u reg=%04x cmd=%08x CX=%u trigger=%u ack=%u reg=%04x cmd=%08x\n",
            (unsigned)status, rails.mx_index, rails.mx.triggered, rails.mx.acknowledged,
            rails.mx.failed_register, rails.mx.command_status, rails.cx_index,
            rails.cx.triggered, rails.cx.acknowledged, rails.cx.failed_register,
            rails.cx.command_status);
    }
    ml_io_unmap(mapping, SM8750_APPS_RSC_DRV2_PROBE_SIZE);
    kfree_data(snapshot, Sm8750CmdDbSize);
    if (stage == 3 && status == Sm8750RpmhReadOk) {
        elden_adreno830_probe_power(&rails);
    }
}
#endif

/* GPUCC requires LOW_SVS on the host MX and CX rails. */
static void
elden_adreno830_rpmh_probe(const void *snapshot)
{
    uint32_t mx_address, cx_address;
    unsigned mx_index, cx_index;
    if (Sm8750CmdDbArcLevel(snapshot, Sm8750CmdDbSize, "mx.lvl", 0x40,
                            &mx_address, &mx_index) != Sm8750CmdDbOk ||
        Sm8750CmdDbArcLevel(snapshot, Sm8750CmdDbSize, "cx.lvl", 0x40,
                            &cx_address, &cx_index) != Sm8750CmdDbOk) {
        printf("ELDEN-A830: RPMh LOW_SVS tables unavailable\n");
        return;
    }
    printf("ELDEN-A830: RPMh LOW_SVS MX=%05x:%u CX=%05x:%u\n",
           mx_address, mx_index, cx_address, cx_index);
    vm_offset_t mapping = io_map(SM8750_APPS_RSC_DRV2_BASE, SM8750_APPS_RSC_DRV2_PROBE_SIZE,
                                 VM_WIMG_IO, VM_PROT_READ, true);
    if (!mapping) {
        printf("ELDEN-A830: RPMh DRV-2 map failed\n");
        return;
    }

    printf("ELDEN-A830: reading RPMh DRV-2 state\n");
    const volatile uint32_t *regs = (const volatile uint32_t *)mapping;
    uint32_t hardware = regs[0];
    uint32_t config = regs[0x0c / 4];
    unsigned major = (hardware >> 16) & 0xff;
    unsigned tcs_count = (config >> 12) & 0x3f;
    unsigned command_count = (config >> 27) & 0x1f;
    printf("ELDEN-A830: RPMh DRV-2 id=%08x config=%08x tcs=%u commands=%u\n",
           hardware, config, tcs_count, command_count);
    if ((major == 2 || major == 3) && tcs_count >= SM8750_APPS_RSC_ACTIVE_TCS &&
        tcs_count <= 16 && command_count >= 3 && command_count <= 16) {
        /* RSC v3 moves the WAIT/CONTROL/STATUS/ENABLE group by 16 bytes. */
        unsigned wait_offset = major == 2 ? 0x10 : 0x20;
        uint32_t irq_enable = regs[SM8750_APPS_RSC_TCS_BASE / 4];
        uint32_t irq_status = regs[(SM8750_APPS_RSC_TCS_BASE + 4) / 4];
        unsigned idle = 0;
        for (unsigned i = 0; i < SM8750_APPS_RSC_ACTIVE_TCS; ++i) {
            unsigned offset = SM8750_APPS_RSC_TCS_BASE + i * SM8750_APPS_RSC_TCS_STRIDE;
            const volatile uint32_t *tcs = regs + (offset + wait_offset) / 4;
            uint32_t wait = tcs[0], control = tcs[1], status = tcs[2], enable = tcs[3];
            if (!wait && !control && status == 1 && !enable && !(irq_status & (1u << i))) {
                idle |= 1u << i;
            }
            printf("ELDEN-A830: RPMh TCS%u wait=%08x control=%08x status=%08x enable=%08x\n",
                   i, wait, control, status, enable);
        }
        printf("ELDEN-A830: RPMh IRQ en=%08x status=%08x idle-snapshot=%x\n",
               irq_enable, irq_status, idle);
    }
    ml_io_unmap(mapping, SM8750_APPS_RSC_DRV2_PROBE_SIZE);
}

static void
elden_adreno830_cmd_db_probe(void)
{
    unsigned enabled = 0, rpmh = 0;
    bool log_resources = PE_parse_boot_argn("elden-adreno830-cmd-db", &enabled, sizeof(enabled)) &&
        enabled == 1;
    bool read_rpmh = PE_parse_boot_argn("elden-adreno830-rpmh", &rpmh, sizeof(rpmh)) && rpmh == 1;
    if (!log_resources && !read_rpmh) {
        return;
    }
    uint32_t *snapshot = elden_adreno830_copy_cmd_db();
    if (!snapshot) {
        return;
    }

    static const struct {
        const char *name;
        unsigned slave;
    } required[] = {
        {"gfx.lvl", Sm8750CmdDbArc}, {"mx.lvl", Sm8750CmdDbArc},
        {"cx.lvl", Sm8750CmdDbArc}, {"gmxc.lvl", Sm8750CmdDbArc},
        {"SH0", Sm8750CmdDbBcm}, {"MC0", Sm8750CmdDbBcm},
        {"ACV", Sm8750CmdDbBcm}, {"CN0", Sm8750CmdDbBcm},
    };
    for (size_t i = 0; log_resources && i < sizeof(required) / sizeof(required[0]); ++i) {
        Sm8750CmdDbResource resource;
        Sm8750CmdDbStatus status = Sm8750CmdDbRead(snapshot, Sm8750CmdDbSize,
            required[i].name, required[i].slave, &resource);
        if (status != Sm8750CmdDbOk) {
            printf("ELDEN-A830: cmd-db %s status=%u\n", required[i].name, (unsigned)status);
            continue;
        }
        uint32_t digest = elden_adreno830_fnv1a(resource.aux, resource.aux_bytes);
        printf("ELDEN-A830: cmd-db %s addr=%05x bytes=%lu fnv1a=%08x\n", required[i].name,
            resource.address, (unsigned long)resource.aux_bytes, digest);
    }
#ifdef ELDEN_ADRENO830_HFI_PROBE
    if (log_resources) {
        elden_adreno830_hfi_probe(snapshot);
    }
#endif
    if (read_rpmh) {
        elden_adreno830_rpmh_probe(snapshot);
    }
    kfree_data(snapshot, Sm8750CmdDbSize);
}

static void
elden_adreno830_gpucc_probe(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("elden-adreno830-gpucc", &enabled, sizeof(enabled)) || enabled != 1) {
        return;
    }
    vm_offset_t page = io_map(SM8750_GPUCC_CX_PAGE, SM8750_GMU_PAGE_BYTES, VM_WIMG_IO, VM_PROT_READ, true);
    if (!page) {
        printf("ELDEN-A830: GPUCC CX map failed\n");
        return;
    }
    const volatile uint32_t *gpucc = (const volatile uint32_t *)page;
    printf("ELDEN-A830: reading GPUCC CX clock and power state\n");
    uint32_t cx = gpucc[SM8750_GPUCC_CX_GDSCR / 4];
    uint32_t hw = gpucc[SM8750_GPUCC_CX_GDS_HW_CTRL / 4];
    uint32_t ahb = gpucc[SM8750_GPUCC_AHB_CBCR / 4];
    uint32_t gmu = gpucc[SM8750_GPUCC_CX_GMU_CBCR / 4];
    uint32_t cxo = gpucc[SM8750_GPUCC_CXO_CBCR / 4];
    uint32_t hub = gpucc[SM8750_GPUCC_HUB_CX_INT_CBCR / 4];
    ml_io_unmap(page, SM8750_GMU_PAGE_BYTES);
    printf("ELDEN-A830: GPUCC CX=%08x HW=%08x AHB=%08x GMU=%08x CXO=%08x HUB=%08x\n",
           cx, hw, ahb, gmu, cxo, hub);
}

typedef struct {
    unsigned blocks;
    size_t bytes;
} EldenGmuProbe;

static int
elden_gmu_count(void *context, EldenGmuRegion region __unused, uint32_t offset __unused,
               const uint8_t *payload __unused, size_t bytes)
{
    EldenGmuProbe *probe = context;
    ++probe->blocks;
    probe->bytes += bytes;
    return 1;
}

static void
elden_adreno830_probe(void)
{
    elden_adreno830_gpucc_probe();
    elden_adreno830_cmd_db_probe();
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("elden-adreno830-probe", &enabled, sizeof(enabled)) || enabled != 1) {
        return;
    }
    EldenGmuProbe probe = {0};
    EldenGmuResult result = EldenAdreno830LoadGmu(ELDEN_ADRENO830_CHIP_ID,
        elden_gmu_image, sizeof(elden_gmu_image), elden_gmu_count, &probe);
    printf("ELDEN-A830: GMU image result=%u blocks=%u bytes=%lu\n",
           (unsigned)result, probe.blocks, (unsigned long)probe.bytes);
}
