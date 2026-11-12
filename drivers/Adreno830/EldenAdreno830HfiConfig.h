/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef ELDEN_ADRENO830_HFI_CONFIG_H
#define ELDEN_ADRENO830_HFI_CONFIG_H

#include "AdrenoHfiA8xx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EldenAdrenoResourceFound,
    EldenAdrenoResourceAbsent,
    EldenAdrenoResourceInvalid,
} EldenAdrenoResourceStatus;

typedef struct {
    uint32_t address;
    const uint8_t *aux;
    size_t aux_bytes;
} EldenAdrenoResource;

typedef struct {
    /* MSM_PARAM_CHIP_ID, including the reported speed-bin code in bits 47:32. */
    uint64_t drm_chip_id;
    const uint64_t *gpu_hz, *gmu_hz; /* Observed enabled OPPs in ascending Hz, without off. */
    size_t gpu_count, gmu_count;
    void *context;
    /* Lookup only in a complete, verified Elden command-db snapshot. Report
     * absence only after searching the complete snapshot. A read error,
     * malformed record or duplicate must return EldenAdrenoResourceInvalid. */
    EldenAdrenoResourceStatus (*find_resource)(void *, const char *name, unsigned slave,
                                                EldenAdrenoResource *);
} EldenAdreno830HfiInputs;

typedef struct {
    AdrenoHfiA8xxTablesConfig tables;
    AdrenoHfiA8xxGpuLevel gx[15];
    AdrenoHfiPerformanceLevel cx[4];
    AdrenoHfiStartupConfig bandwidth_storage;
} EldenAdreno830HfiConfig;

typedef enum {
    EldenAdrenoHfiSuccess,
    EldenAdrenoHfiArgument,
    EldenAdrenoHfiIdentity,
    EldenAdrenoHfiOppMismatch,
    EldenAdrenoHfiCommandDb,
    EldenAdrenoHfiVotes,
} EldenAdreno830HfiStatus;

/* Build the two HFI tables for the saved 0x44050000 Elden Linux profile.
 * The caller verifies the same-device identity, active OPPs and provenance
 * of the command-db snapshot. Inputs and output must be disjoint. Resource
 * aux bytes stay readable and immutable for the duration of the call.
 * The result owns its table storage and must stay at its original address.
 * Refusal leaves the output untouched. No MMIO, power votes or HFI I/O. */
EldenAdreno830HfiStatus EldenAdreno830BuildHfiConfig(const EldenAdreno830HfiInputs *,
                                                   EldenAdreno830HfiConfig *);

#ifdef __cplusplus
}
#endif
#endif
