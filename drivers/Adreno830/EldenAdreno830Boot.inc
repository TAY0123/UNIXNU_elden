/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <vm/memory_types.h>
#include "AdrenoFirmware.c"
#include "EldenAdreno830.c"
#include "EldenAdreno830GmuImage.h"

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
