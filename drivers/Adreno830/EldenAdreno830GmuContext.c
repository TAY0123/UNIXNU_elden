/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "EldenAdreno830GmuContext.h"
#include "ArmSmmu4KTableMemory.h"
#include "Sm8750AdrenoResources.h"

enum {
    GmuData = ArmSmmu4KRead | ArmSmmu4KWrite,
};

/* Keep the same lowest-free log and HFI IOVAs as NativeAdrenoGmuAllocator.
 * The A8xx fixed allocations use privileged mappings. Only icache executes. */
static const struct {
    uint32_t iova;
    size_t bytes;
    unsigned permissions;
} gmu_layout[EldenGmuMemoryCount] = {
    [EldenGmuMemoryIcache] = {SM8750_GMU_ICACHE_IOVA, SM8750_GMU_ICACHE_BYTES,
                             GmuData | ArmSmmu4KPrivileged | ArmSmmu4KExecute},
    [EldenGmuMemoryScratch] = {SM8750_GMU_SCRATCH_IOVA, SM8750_GMU_SCRATCH_BYTES,
                              GmuData | ArmSmmu4KPrivileged},
    [EldenGmuMemoryLog] = {SM8750_GMU_LOG_IOVA, SM8750_GMU_LOG_BYTES, GmuData},
    [EldenGmuMemoryHfi] = {SM8750_GMU_HFI_IOVA, SM8750_GMU_HFI_BYTES, GmuData},
    [EldenGmuMemoryDebug] = {SM8750_GMU_DEBUG_IOVA, SM8750_GMU_DEBUG_BYTES,
                            GmuData | ArmSmmu4KPrivileged},
};

static int overlaps(uint64_t a, size_t a_bytes, uint64_t b, size_t b_bytes)
{
    return a <= b ? b - a < a_bytes : a - b < b_bytes;
}

static int valid_cpu_extent(uintptr_t base, size_t bytes)
{
    return base && !(base & (SM8750_GMU_PAGE_BYTES - 1)) && bytes <= UINTPTR_MAX - base;
}

ArmSmmu4KStatus EldenAdreno830PrepareGmuContext(
    const EldenGmuAllocation allocations[EldenGmuMemoryCount], uint64_t *table_cpu,
    uint64_t table_physical, size_t table_capacity, unsigned output_bits, EldenGmuContext *context)
{
    if (!context) return ArmSmmu4KArgument;
    *context = (EldenGmuContext){0};
    if (!allocations || !table_capacity || table_capacity > SIZE_MAX / SM8750_GMU_PAGE_BYTES) {
        return ArmSmmu4KArgument;
    }
    size_t table_bytes = table_capacity * SM8750_GMU_PAGE_BYTES;
    if (!table_physical || !valid_cpu_extent((uintptr_t)table_cpu, table_bytes)) {
        return ArmSmmu4KBadRange;
    }

    EldenGmuContext prepared = {
        .translation = {.input_bits = 32, .output_bits = output_bits, .table_physical = table_physical},
        .table_cpu = table_cpu, .table_capacity = table_capacity,
    };
    for (unsigned i = 0; i < EldenGmuMemoryCount; ++i) {
        const EldenGmuAllocation *allocation = &allocations[i];
        uintptr_t cpu = (uintptr_t)allocation->cpu;
        if (allocation->bytes != gmu_layout[i].bytes || !allocation->physical ||
            !valid_cpu_extent(cpu, allocation->bytes)) {
            return ArmSmmu4KBadRange;
        }
        if (overlaps(cpu, allocation->bytes, (uintptr_t)table_cpu, table_bytes)) {
            return ArmSmmu4KOverlap;
        }
        for (unsigned j = 0; j < i; ++j) {
            const EldenGmuAllocation *prior = &allocations[j];
            if (overlaps(cpu, allocation->bytes, (uintptr_t)prior->cpu, prior->bytes) ||
                overlaps(allocation->physical, allocation->bytes, prior->physical, prior->bytes)) {
                return ArmSmmu4KOverlap;
            }
        }
        prepared.ranges[i] = (ArmSmmu4KRange){
            .iova = gmu_layout[i].iova, .physical = allocation->physical, .bytes = allocation->bytes,
            .permissions = gmu_layout[i].permissions, .writeback = 0, .shareability = 2,
        };
        prepared.buffers[i] = (EldenGmuBuffer){allocation->cpu, allocation->bytes, gmu_layout[i].iova};
        prepared.translation.leaf_budget += allocation->bytes / SM8750_GMU_PAGE_BYTES;
    }

    ArmSmmu4KStatus status = ArmSmmu4KBuild(&prepared.translation, prepared.ranges, EldenGmuMemoryCount,
                                          table_cpu, table_capacity, &prepared.tables);
    if (status == ArmSmmu4KSuccess) *context = prepared;
    return status;
}

ArmSmmu4KStatus EldenAdreno830ObserveGmuContext(
    const EldenGmuContext *context, const ArmSmmu4KIo *smmu,
    const ArmSmmu4KStreamState *stream, const ArmSmmu4KContextConfig *config,
    uint8_t *visited, size_t visited_bytes, EldenGmuContextObservation *observation)
{
    if (!observation) return ArmSmmu4KArgument;
    *observation = (EldenGmuContextObservation){0};
    if (!context || !smmu || smmu->register_bytes != SM8750_GPU_SMMU_BYTES ||
        !stream || !stream->attached || stream->poisoned || stream->count != 1 ||
        stream->streams[0].sid != SM8750_GMU_STREAM_ID || !config || !config->asid ||
        config->lower != &context->translation || config->lower_tables != &context->tables ||
        config->upper || config->upper_tables || config->ttbr0_switched ||
        !context->tables.table_pages || context->tables.table_pages > SIZE_MAX / 512 ||
        !visited || visited_bytes < context->table_capacity) {
        return ArmSmmu4KArgument;
    }

    ArmSmmu4KTableMemory pool = {
        .count = 1, .pools = {{context->table_cpu, context->translation.table_physical,
                               context->table_capacity}},
    };
    ArmSmmu4KAuditRegion lower = {
        .ranges = context->ranges, .range_count = EldenGmuMemoryCount,
        .capacity_pages = context->table_capacity, .read_budget = context->tables.table_pages * 512,
        .visited = visited, .visited_bytes = visited_bytes,
    };
    ArmSmmu4KAuditConfig audit = {.lower = &lower};
    ArmSmmu4KStatus status = ArmSmmu4KBindTableMemory(&pool, &audit.io);
    if (status != ArmSmmu4KSuccess) return status;
    if (overlaps((uintptr_t)visited, context->table_capacity, (uintptr_t)context->table_cpu,
                 context->table_capacity * SM8750_GMU_PAGE_BYTES)) {
        return ArmSmmu4KOverlap;
    }
    for (unsigned i = 0; i < EldenGmuMemoryCount; ++i) {
        const EldenGmuBuffer *buffer = &context->buffers[i];
        if (overlaps((uintptr_t)visited, context->table_capacity, (uintptr_t)buffer->cpu, buffer->bytes)) {
            return ArmSmmu4KOverlap;
        }
    }

    status = ArmSmmu4KObserveStreams(smmu, stream, config, &observation->streams_before);
    if (status != ArmSmmu4KSuccess) return status;
    status = ArmSmmu4KObserveAuditedContext(smmu, stream->bank, config, &audit, &observation->tables);
    if (status != ArmSmmu4KSuccess) return status;
    return ArmSmmu4KObserveStreams(smmu, stream, config, &observation->streams_after);
}
