/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef ELDEN_ADRENO830_H
#define ELDEN_ADRENO830_H

#include "AdrenoFirmware.h"

#define ELDEN_ADRENO830_CHIP_ID UINT32_C(0x44050000)

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
} EldenGmuResult;

EldenGmuResult EldenAdreno830LoadGmu(uint32_t chip_id, const uint8_t *image, size_t bytes,
                                    EldenGmuVisit visit, void *context);

#endif
