/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AdrenoFirmware.c"
#include "EldenAdreno830.c"
#include "EldenAdreno830GmuImage.h"

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
