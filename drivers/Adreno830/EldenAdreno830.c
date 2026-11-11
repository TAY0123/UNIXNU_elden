/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "EldenAdreno830.h"

/* SM8750 GMU address space. The instruction cache and scratch windows are
 * GMU IOVAs. The two TCM windows are backed by GMU registers. */
static const AdrenoFirmwareRegion regions[] = {
    {0x00000000, 0x00004000, EldenGmuItcm},
    {0x00004000, 0x00ffc000, EldenGmuIcache},
    {0x10004000, 0x00004000, EldenGmuDtcm},
    {0x60000000, 0x00004000, EldenGmuScratch},
};

EldenGmuResult EldenAdreno830LoadGmu(uint32_t chip_id, const uint8_t *image, size_t bytes,
                                    EldenGmuVisit visit, void *context)
{
    AdrenoFirmwareBlock blocks[32];
    size_t count;
    if (chip_id != ELDEN_ADRENO830_CHIP_ID || !visit ||
        AdrenoFirmwarePlanGmu(image, bytes, regions, sizeof(regions) / sizeof(regions[0]),
                              blocks, sizeof(blocks) / sizeof(blocks[0]), &count) != AdrenoFirmwareSuccess) {
        return EldenGmuInvalidImage;
    }

    int has_entry = 0;
    for (size_t i = 0; i < count; ++i) {
        const AdrenoFirmwareBlock *block = &blocks[i];
        if (!block->bytes) continue;
        if (block->address == 0) has_entry = 1;
        for (size_t j = 0; j < i; ++j) {
            const AdrenoFirmwareBlock *prior = &blocks[j];
            if (prior->bytes && block->address < (uint64_t)prior->address + prior->bytes &&
                prior->address < (uint64_t)block->address + block->bytes) {
                return EldenGmuInvalidImage;
            }
        }
    }
    if (!has_entry) return EldenGmuInvalidImage;

    for (size_t i = 0; i < count; ++i) {
        const AdrenoFirmwareBlock *block = &blocks[i];
        if (!block->bytes) continue;
        const AdrenoFirmwareRegion *region = &regions[block->region_identifier];
        if (!visit(context, (EldenGmuRegion)region->identifier, block->address - region->address,
                   image + block->payload_offset, block->bytes)) {
            return EldenGmuVisitorFailed;
        }
    }
    return EldenGmuSuccess;
}
