/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "EldenAdreno830.h"
#include "Sm8750AdrenoResources.h"

_Static_assert(SM8750_ADRENO_GMU_DTCM + SM8750_GMU_TCM_BYTES <= SM8750_ADRENO_GMU_BYTES,
               "DTCM must fit the GMU aperture");
_Static_assert(SM8750_ADRENO_GMU_CM3_SYSRESET + 4 <= SM8750_ADRENO_GMU_BYTES,
               "CM3 reset must fit the GMU aperture");

static const AdrenoFirmwareRegion elden_gmu_regions[] = {
    [EldenGmuItcm] = {SM8750_GMU_ITCM_ADDRESS, SM8750_GMU_TCM_BYTES, EldenGmuItcm},
    [EldenGmuIcache] = {SM8750_GMU_ICACHE_IOVA, SM8750_GMU_ICACHE_BYTES, EldenGmuIcache},
    [EldenGmuDtcm] = {SM8750_GMU_DTCM_ADDRESS, SM8750_GMU_TCM_BYTES, EldenGmuDtcm},
    [EldenGmuScratch] = {SM8750_GMU_SCRATCH_IOVA, SM8750_GMU_SCRATCH_BYTES, EldenGmuScratch},
};

enum { EldenGmuMaxBlocks = 32 };

static int
elden_gmu_plan(uint32_t chip_id, const uint8_t *image, size_t bytes,
               AdrenoFirmwareBlock blocks[EldenGmuMaxBlocks], size_t *count)
{
    if (chip_id != ELDEN_ADRENO830_CHIP_ID || !image || !bytes ||
        bytes - 1 > UINTPTR_MAX - (uintptr_t)image ||
        AdrenoFirmwarePlanGmu(image, bytes, elden_gmu_regions,
                              sizeof(elden_gmu_regions) / sizeof(elden_gmu_regions[0]),
                              blocks, EldenGmuMaxBlocks, count) != AdrenoFirmwareSuccess) {
        return 0;
    }

    int has_entry = 0;
    for (size_t i = 0; i < *count; ++i) {
        const AdrenoFirmwareBlock *block = &blocks[i];
        if (!block->bytes) continue;
        if (block->address == SM8750_GMU_ITCM_ADDRESS) has_entry = 1;
        for (size_t j = 0; j < i; ++j) {
            const AdrenoFirmwareBlock *prior = &blocks[j];
            if (prior->bytes && block->address < (uint64_t)prior->address + prior->bytes &&
                prior->address < (uint64_t)block->address + block->bytes) {
                return 0;
            }
        }
    }
    return has_entry;
}

EldenGmuResult EldenAdreno830LoadGmu(uint32_t chip_id, const uint8_t *image, size_t bytes,
                                    EldenGmuVisit visit, void *context)
{
    AdrenoFirmwareBlock blocks[EldenGmuMaxBlocks];
    size_t count;
    if (!visit || !elden_gmu_plan(chip_id, image, bytes, blocks, &count)) {
        return EldenGmuInvalidImage;
    }

    for (size_t i = 0; i < count; ++i) {
        const AdrenoFirmwareBlock *block = &blocks[i];
        if (!block->bytes) continue;
        const AdrenoFirmwareRegion *region = &elden_gmu_regions[block->region_identifier];
        if (!visit(context, (EldenGmuRegion)region->identifier, block->address - region->address,
                   image + block->payload_offset, block->bytes)) {
            return EldenGmuVisitorFailed;
        }
    }
    return EldenGmuSuccess;
}

static int
elden_gmu_overlap(const void *first, size_t first_bytes, const void *second, size_t second_bytes)
{
    uintptr_t a = (uintptr_t)first;
    uintptr_t b = (uintptr_t)second;
    return first_bytes && second_bytes && (a <= b ? b - a < first_bytes : a - b < second_bytes);
}

static int
elden_gmu_buffer_valid(const EldenGmuBuffer *buffer, EldenGmuRegion region,
                      const uint8_t *image, size_t bytes)
{
    if (!buffer->bytes) return !buffer->cpu && !buffer->iova;

    const AdrenoFirmwareRegion *window = &elden_gmu_regions[region];
    uintptr_t cpu = (uintptr_t)buffer->cpu;
    return cpu && buffer->iova == window->address && buffer->bytes <= window->bytes &&
        !((cpu | buffer->bytes) & (SM8750_GMU_PAGE_BYTES - 1)) &&
        buffer->bytes - 1 <= UINTPTR_MAX - cpu &&
        !elden_gmu_overlap(buffer->cpu, buffer->bytes, image, bytes);
}

static const EldenGmuBuffer *
elden_gmu_backing(const EldenGmuTransfer *transfer, uint32_t region)
{
    switch (region) {
    case EldenGmuIcache: return &transfer->icache;
    case EldenGmuScratch: return &transfer->scratch;
    default: return NULL;
    }
}

static uint32_t
elden_gmu_tcm_offset(uint32_t region, uint32_t offset)
{
    return (region == EldenGmuItcm ? SM8750_ADRENO_GMU_ITCM : SM8750_ADRENO_GMU_DTCM) + offset;
}

static uint32_t
elden_gmu_word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static EldenGmuResult
elden_gmu_check_reset(const EldenGmuTransfer *transfer)
{
    uint32_t reset;
    if (transfer->read32(transfer->context, SM8750_ADRENO_GMU_CM3_SYSRESET, &reset)) {
        return EldenGmuIoFailed;
    }
    return reset == 1 ? EldenGmuSuccess : EldenGmuResetNotAsserted;
}

EldenGmuResult EldenAdreno830TransferGmu(uint32_t chip_id, const uint8_t *image, size_t bytes,
                                       const EldenGmuTransfer *transfer)
{
    AdrenoFirmwareBlock blocks[EldenGmuMaxBlocks];
    size_t count;
    if (!elden_gmu_plan(chip_id, image, bytes, blocks, &count)) return EldenGmuInvalidImage;
    if (!transfer || transfer->mmio_bytes != SM8750_ADRENO_GMU_BYTES ||
        !transfer->read32 || !transfer->write32 || !transfer->sync_for_device || !transfer->barrier ||
        !elden_gmu_buffer_valid(&transfer->icache, EldenGmuIcache, image, bytes) ||
        !elden_gmu_buffer_valid(&transfer->scratch, EldenGmuScratch, image, bytes) ||
        elden_gmu_overlap(transfer->icache.cpu, transfer->icache.bytes,
                         transfer->scratch.cpu, transfer->scratch.bytes)) {
        return EldenGmuInvalidTransfer;
    }
    for (size_t i = 0; i < count; ++i) {
        const AdrenoFirmwareBlock *block = &blocks[i];
        if (!block->bytes) continue;
        const EldenGmuBuffer *backing = elden_gmu_backing(transfer, block->region_identifier);
        uint32_t offset = block->address - elden_gmu_regions[block->region_identifier].address;
        if (backing && (offset > backing->bytes || block->bytes > backing->bytes - offset)) {
            return EldenGmuInvalidTransfer;
        }
    }

    EldenGmuResult result = elden_gmu_check_reset(transfer);
    if (result != EldenGmuSuccess) return result;

    for (size_t i = 0; i < count; ++i) {
        const AdrenoFirmwareBlock *block = &blocks[i];
        if (!block->bytes) continue;
        const EldenGmuBuffer *backing = elden_gmu_backing(transfer, block->region_identifier);
        uint32_t offset = block->address - elden_gmu_regions[block->region_identifier].address;
        const uint8_t *payload = image + block->payload_offset;
        if (backing) {
            uint8_t *destination = backing->cpu + offset;
            __builtin_memcpy(destination, payload, block->bytes);
            if (transfer->sync_for_device(transfer->context, destination, block->bytes)) {
                return EldenGmuIoFailed;
            }
            continue;
        }
        uint32_t reg = elden_gmu_tcm_offset(block->region_identifier, offset);
        for (uint32_t at = 0; at < block->bytes; at += 4) {
            if (transfer->write32(transfer->context, reg + at, elden_gmu_word(payload + at))) {
                return EldenGmuIoFailed;
            }
        }
    }
    if (transfer->barrier(transfer->context)) return EldenGmuIoFailed;

    for (size_t i = 0; i < count; ++i) {
        const AdrenoFirmwareBlock *block = &blocks[i];
        if (!block->bytes || elden_gmu_backing(transfer, block->region_identifier)) continue;
        uint32_t offset = block->address - elden_gmu_regions[block->region_identifier].address;
        uint32_t reg = elden_gmu_tcm_offset(block->region_identifier, offset);
        const uint8_t *payload = image + block->payload_offset;
        for (uint32_t at = 0; at < block->bytes; at += 4) {
            uint32_t actual;
            if (transfer->read32(transfer->context, reg + at, &actual)) return EldenGmuIoFailed;
            if (actual != elden_gmu_word(payload + at)) return EldenGmuTcmMismatch;
        }
    }
    return elden_gmu_check_reset(transfer);
}
