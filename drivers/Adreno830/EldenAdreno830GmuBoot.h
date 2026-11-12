/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef ELDEN_ADRENO830_GMU_BOOT_H
#define ELDEN_ADRENO830_GMU_BOOT_H

#include "AdrenoHfi.h"
#include "EldenAdreno830GmuContext.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EldenGmuBootIdle,
    EldenGmuBootTransfer,
    EldenGmuBootConfigure,
    EldenGmuBootFirmwareReady,
    EldenGmuBootHfiReady,
    EldenGmuBootRunning,
} EldenGmuBootStage;

typedef enum {
    EldenGmuBootSuccess,
    EldenGmuBootInvalid,
    EldenGmuBootAlreadyAttempted,
    EldenGmuBootTransferFailed,
    EldenGmuBootIoFailed,
    EldenGmuBootUnhealthy,
    EldenGmuBootUnexpectedState,
    EldenGmuBootTimeout,
} EldenGmuBootResult;

typedef struct {
    EldenGmuTransfer transfer_io;
    int (*write_gpu32)(void *context, uint32_t offset, uint32_t value);
    int (*check_health)(void *context);
    int (*now_us)(void *context, uint64_t *time);
    int (*wait_us)(void *context, unsigned delay);
} EldenGmuBootServices;

typedef struct {
    AdrenoHfi hfi;
    EldenGmuBootStage stage;
    EldenGmuResult transfer_result;
    uint32_t core_version;
    uint32_t reg;
    uint32_t value;
    unsigned gpu_register;
    unsigned reset_reasserted;
} EldenGmuBoot;

/* Cold boot the pinned Elden GMU image using a zero-initialized boot object.
 * The owner serializes calls and supplies an already observed live SID 5
 * context, powered CX/GMU, working clocks, boot bandwidth, secure setup and
 * an awake RSCC with the FAL veto in place. CM3 must be held in reset and the
 * host owns both GMU IRQ routes in polling mode with old faults cleared.
 * check_health checks power and platform faults (including SMMU faults)
 * promptly without changing interrupt ownership. MMIO callbacks must fail
 * safely if the aperture is inaccessible during fault recovery. All
 * callbacks return zero on success. write_gpu32 uses GPU-relative byte
 * offsets. The backing descriptors inside transfer_io are ignored.
 *
 * The context backing is disjoint, pinned and coherent for HFI in both
 * directions. The immutable image, services, boot object and context must
 * be disjoint from the DMA allocations. Services and resources stay valid
 * while HFI can be used. On success, boot->hfi is ready for the startup
 * tables and the owner handles response notification and fault polling.
 * No IRQ acknowledgments are consumed here. A failed attempt is terminal
 * for this object. On failure after attempting CM3 release, boot reports
 * whether asserting reset and reading it back succeeded. The owner must
 * establish quiescence and drain translations before releasing resources.
 */
EldenGmuBootResult EldenAdreno830BootGmu(EldenGmuBoot *boot, uint32_t chip_id,
    const uint8_t *image, size_t bytes, const EldenGmuContext *memory,
    const EldenGmuBootServices *services);

#ifdef __cplusplus
}
#endif
#endif
