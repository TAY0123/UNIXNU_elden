/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef ELDEN_ADRENO830_H
#define ELDEN_ADRENO830_H

#include "AdrenoFirmware.h"

#define ELDEN_ADRENO830_CHIP_ID UINT32_C(0x44050000)

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EldenGmuItcm,
    EldenGmuIcache,
    EldenGmuDtcm,
    EldenGmuScratch,
} EldenGmuRegion;

/* The image must remain immutable for the duration of the call. The visitor
 * runs only after validation of the complete image. A visitor that writes
 * hardware must keep the GMU in reset until all writes have succeeded. */
typedef int (*EldenGmuVisit)(void *context, EldenGmuRegion region, uint32_t offset,
                            const uint8_t *payload, size_t bytes);

typedef enum {
    EldenGmuSuccess,
    EldenGmuInvalidImage,
    EldenGmuVisitorFailed,
    EldenGmuInvalidTransfer,
    EldenGmuResetNotAsserted,
    EldenGmuIoFailed,
    EldenGmuTcmMismatch,
} EldenGmuResult;

EldenGmuResult EldenAdreno830LoadGmu(uint32_t chip_id, const uint8_t *image, size_t bytes,
                                    EldenGmuVisit visit, void *context);

typedef struct {
    uint8_t *cpu;
    size_t bytes;
    uint64_t iova;
} EldenGmuBuffer;

typedef struct {
    void *context;
    size_t mmio_bytes;
    int (*read32)(void *context, uint32_t offset, uint32_t *value);
    int (*write32)(void *context, uint32_t offset, uint32_t value);
    int (*sync_for_device)(void *context, void *cpu, size_t bytes);
    int (*barrier)(void *context);
    EldenGmuBuffer icache;
    EldenGmuBuffer scratch;
} EldenGmuTransfer;

/* Use the authenticated, immutable image and chip ID from trusted discovery.
 * Caller owns the GMU, holds power/clocks and CM3 reset for the whole call,
 * and pins the backing in the GMU's 4 KiB SMMU context. Backing descriptors
 * cover page-aligned writable CPU mappings of the fixed IOVA windows.
 * An unused buffer may be zero-initialized. MMIO offsets are bytes relative
 * to SM8750_ADRENO_GMU_BASE. I/O callbacks return zero on success.
 * sync_for_device publishes CPU writes to the GMU and barrier orders I/O.
 * Input and destinations are preflighted before I/O. A failure during
 * transfer can leave partial contents. Caller keeps reset asserted and the
 * resources owned until its recovery sequence completes. */
EldenGmuResult EldenAdreno830TransferGmu(uint32_t chip_id, const uint8_t *image, size_t bytes,
                                       const EldenGmuTransfer *transfer);

#ifdef __cplusplus
}
#endif

#endif
