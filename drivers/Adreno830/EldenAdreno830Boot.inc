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

static void
elden_adreno830_cmd_db_probe(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("elden-adreno830-cmd-db", &enabled, sizeof(enabled)) || enabled != 1) {
        return;
    }
    uint32_t *snapshot = kalloc_data(Sm8750CmdDbSize, Z_WAITOK);
    if (!snapshot) {
        printf("ELDEN-A830: command database allocation failed\n");
        return;
    }
    vm_offset_t mapping = io_map(Sm8750CmdDbPhysicalBase, Sm8750CmdDbSize, VM_WIMG_IO,
                                 VM_PROT_READ, true);
    if (!mapping) {
        kfree_data(snapshot, Sm8750CmdDbSize);
        printf("ELDEN-A830: command database map failed\n");
        return;
    }
    const volatile uint32_t *db = (const volatile uint32_t *)mapping;
    if (db[1] != Sm8750CmdDbMagic) {
        ml_io_unmap(mapping, Sm8750CmdDbSize);
        kfree_data(snapshot, Sm8750CmdDbSize);
        printf("ELDEN-A830: command database magic mismatch\n");
        return;
    }
    for (size_t i = 0; i < Sm8750CmdDbSize / sizeof(uint32_t); ++i) {
        snapshot[i] = db[i];
    }
    ml_io_unmap(mapping, Sm8750CmdDbSize);

    static const struct {
        const char *name;
        unsigned slave;
    } required[] = {
        {"gfx.lvl", Sm8750CmdDbArc}, {"mx.lvl", Sm8750CmdDbArc},
        {"cx.lvl", Sm8750CmdDbArc}, {"gmxc.lvl", Sm8750CmdDbArc},
        {"SH0", Sm8750CmdDbBcm}, {"MC0", Sm8750CmdDbBcm},
        {"ACV", Sm8750CmdDbBcm}, {"CN0", Sm8750CmdDbBcm},
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
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
    elden_adreno830_hfi_probe(snapshot);
#endif
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
