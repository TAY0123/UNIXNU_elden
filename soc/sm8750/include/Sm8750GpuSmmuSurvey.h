/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SM8750_GPU_SMMU_SURVEY_H
#define SM8750_GPU_SMMU_SURVEY_H

#include <stdint.h>

typedef uint32_t (*Sm8750GpuSmmuRead)(void *cookie, uint32_t offset);

typedef struct {
    uint32_t smr;
    uint32_t s2cr;
    unsigned valid;
} Sm8750GpuSmmuRoute;

typedef struct {
    uint32_t id0, id1, id2, scr0, sacr, global_fault;
    unsigned slots, banks, stage2_banks;
    unsigned gmu_matches, invalid_slots, invalid_fault_slots;
    unsigned gmu_bank_routes;
    uint32_t gmu_bank_offset, gmu_sctlr, gmu_actlr, gmu_fsr;
    Sm8750GpuSmmuRoute routes[255];
} Sm8750GpuSmmuSurvey;

/* Observe inherited state without writing registers, claiming a stream,
 * or interpreting an invalid slot as available for use. The caller supplies
 * the full powered GPU SMMU aperture. Returns 0 for an unsupported layout. */
int Sm8750GpuSmmuObserve(Sm8750GpuSmmuRead read32, void *cookie, Sm8750GpuSmmuSurvey *survey);

#endif
