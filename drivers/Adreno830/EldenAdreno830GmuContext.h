/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef ELDEN_ADRENO830_GMU_CONTEXT_H
#define ELDEN_ADRENO830_GMU_CONTEXT_H

#include "EldenAdreno830.h"
#include "ArmSmmu4KStreams.h"
#include "ArmSmmu4KAudit.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EldenGmuMemoryIcache,
    EldenGmuMemoryScratch,
    EldenGmuMemoryLog,
    EldenGmuMemoryHfi,
    EldenGmuMemoryDebug,
    EldenGmuMemoryCount,
} EldenGmuMemoryRegion;

typedef struct {
    uint8_t *cpu;
    size_t bytes;
    uint64_t physical;
} EldenGmuAllocation;

typedef struct {
    ArmSmmu4KConfig translation;
    ArmSmmu4KResult tables;
    ArmSmmu4KRange ranges[EldenGmuMemoryCount];
    EldenGmuBuffer buffers[EldenGmuMemoryCount];
    const volatile uint64_t *table_cpu;
    size_t table_capacity;
} EldenGmuContext;

typedef struct {
    ArmSmmu4KStreamResult streams_before;
    ArmSmmu4KAuditResult tables;
    ArmSmmu4KStreamResult streams_after;
} EldenGmuContextObservation;

/* Build inactive lower stage-1 tables for the five A830 GMU buffers. The
 * caller provides disjoint, contiguous, pinned and 4 KiB-aligned allocations
 * with verified CPU/physical identities. The entire table pool must also be
 * disjoint. Inputs, output and backing must be separate and remain owned.
 * No MMIO, cache publication or SMMU activation. On failure the context is
 * empty and both the table pool and payloads are untouched. */
ArmSmmu4KStatus EldenAdreno830PrepareGmuContext(
    const EldenGmuAllocation allocations[EldenGmuMemoryCount], uint64_t *table_cpu,
    uint64_t table_physical, size_t table_capacity, unsigned output_bits, EldenGmuContext *context);

/* The platform must already hold an exclusive GMU, SMMU slot, bank and ASID
 * lease, quiesce the GMU, own and publish the tables, enable its clocks and
 * install fault handlers. Use the full SM8750 GPU SMMU register window.
 * The supplied stage-1 config points to this context's lower table pair.
 * The caller uses ArmSmmu4K to attach the sole exact SID 5 route first.
 * This checks the exact live route before and after auditing the table graph
 * from the observed TTBR. The entire pool needs one byte/page of scratch.
 * Scratch, inputs and output must be disjoint. All storage remains owned and
 * the caller holds the lease on every result. */
ArmSmmu4KStatus EldenAdreno830ObserveGmuContext(
    const EldenGmuContext *context, const ArmSmmu4KIo *smmu,
    const ArmSmmu4KStreamState *stream, const ArmSmmu4KContextConfig *config,
    uint8_t *visited, size_t visited_bytes, EldenGmuContextObservation *observation);

#ifdef __cplusplus
}
#endif
#endif
