/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "EldenAdreno830GmuContext.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { SmmuBytes = 0x40000, ContextBank = 2, ContextOffset = 0x22000, Slot = 3, Capacity = 16 };
static _Alignas(4096) uint8_t icache[0xffc000], scratch[0x4000], log_buffer[0x4000], hfi[0x4000], debug[0x7000];
static _Alignas(4096) uint64_t tables[Capacity * 512];
static EldenGmuAllocation allocations[] = {
    {icache, sizeof(icache), 0x90000000}, {scratch, sizeof(scratch), 0x91000000},
    {log_buffer, sizeof(log_buffer), 0x91004000}, {hfi, sizeof(hfi), 0x91008000},
    {debug, sizeof(debug), 0x9100c000},
};
static uint32_t registers[SmmuBytes / 4];
static uint8_t visited[Capacity];
static unsigned reads, barriers, change_route_at_barrier;

static int read32(void *cookie, size_t offset, uint32_t *value)
{
    assert(cookie == registers && !(offset & 3) && offset <= SmmuBytes - 4);
    ++reads;
    *value = registers[offset / 4];
    return 0;
}

static int read64(void *cookie, size_t offset, uint64_t *value)
{
    assert(cookie == registers && !(offset & 7) && offset <= SmmuBytes - 8);
    ++reads;
    memcpy(value, registers + offset / 4, sizeof(*value));
    return 0;
}

static int write32(void *cookie, size_t offset, uint32_t value)
{
    (void)cookie;
    (void)offset;
    (void)value;
    assert(!"unexpected 32-bit register write");
    return -1;
}

static int write64(void *cookie, size_t offset, uint64_t value)
{
    (void)cookie;
    (void)offset;
    (void)value;
    assert(!"unexpected 64-bit register write");
    return -1;
}

static int barrier(void *cookie)
{
    assert(cookie == registers);
    if (++barriers == change_route_at_barrier) registers[0xc00 / 4 + Slot] ^= 1;
    return 0;
}

static uint64_t *pte(uint32_t iova)
{
    const uint64_t address_mask = UINT64_C(0x0000fffffffff000);
    const uint64_t table_physical = 0x92000000;
    uint64_t level_one = tables[iova >> 30] & address_mask;
    assert(level_one >= table_physical && level_one - table_physical < sizeof(tables));
    uint64_t level_two = tables[(level_one - table_physical) / 8 + ((iova >> 21) & 511)] & address_mask;
    assert(level_two >= table_physical && level_two - table_physical < sizeof(tables));
    return &tables[(level_two - table_physical) / 8 + ((iova >> 12) & 511)];
}

static ArmSmmu4KStreamState set_registers(const ArmSmmu4KContextConfig *config)
{
    ArmSmmu4KContextConfig actual = *config;
    registers[0x20 / 4] = actual.id0 = 0x48000808;
    registers[0x24 / 4] = 0x40000010;
    registers[0x28 / 4] = actual.id2 = 0x1433;
    registers[0] = 0x406;
    ArmSmmu4KContextImage image;
    assert(ArmSmmu4KEncodeContext(&actual, &image) == ArmSmmu4KSuccess);
    registers[0x1000 / 4 + ContextBank] = image.cbar;
    registers[0x1800 / 4 + ContextBank] = image.cba2r;
    registers[ContextOffset / 4] = image.sctlr;
    registers[(ContextOffset + 0x10) / 4] = image.tcr2;
    registers[(ContextOffset + 0x30) / 4] = image.tcr;
    registers[(ContextOffset + 0x38) / 4] = image.mair0;
    memcpy(registers + (ContextOffset + 0x20) / 4, &image.ttbr0, sizeof(image.ttbr0));
    memcpy(registers + (ContextOffset + 0x28) / 4, &image.ttbr1, sizeof(image.ttbr1));
    registers[0x800 / 4 + Slot] = 0x80000005;
    registers[0xc00 / 4 + Slot] = ContextBank;
    return (ArmSmmu4KStreamState){
        .attempted = 1, .attached = 1, .count = 1, .bank = ContextBank,
        .id0 = actual.id0, .scr0 = registers[0], .streams = {{Slot, 5}},
    };
}

int main(void)
{
    const uint64_t table_physical = 0x92000000;
    EldenGmuContext context;
    memset(tables, 0xa5, sizeof(tables));
    allocations[EldenGmuMemoryLog].physical = allocations[EldenGmuMemoryHfi].physical;
    assert(EldenAdreno830PrepareGmuContext(allocations, tables, table_physical, Capacity, 40, &context) == ArmSmmu4KOverlap);
    allocations[EldenGmuMemoryLog].physical = 0x91004000;
    allocations[EldenGmuMemoryScratch].cpu = (uint8_t *)tables;
    assert(EldenAdreno830PrepareGmuContext(allocations, tables, table_physical, Capacity, 40, &context) == ArmSmmu4KOverlap);
    allocations[EldenGmuMemoryScratch].cpu = scratch;
    --allocations[EldenGmuMemoryDebug].bytes;
    assert(EldenAdreno830PrepareGmuContext(allocations, tables, table_physical, Capacity, 40, &context) == ArmSmmu4KBadRange);
    ++allocations[EldenGmuMemoryDebug].bytes;
    assert(EldenAdreno830PrepareGmuContext(allocations, tables, table_physical, 1, 40, &context) == ArmSmmu4KCapacity);
    assert(!context.table_cpu);
    for (size_t i = 0; i < sizeof(tables); ++i) assert(((uint8_t *)tables)[i] == 0xa5);

    assert(EldenAdreno830PrepareGmuContext(allocations, tables, table_physical, Capacity, 40, &context) == ArmSmmu4KSuccess);
    assert(context.tables.table_pages == 13 && context.tables.leaf_pages == 4111);
    assert(context.buffers[EldenGmuMemoryHfi].iova == 0x60008000);
    assert(context.buffers[EldenGmuMemoryHfi].cpu == hfi);
    assert(context.buffers[EldenGmuMemoryHfi].bytes == 0x4000);
    const uint32_t starts[] = {0x4000, 0x60000000, 0x60004000, 0x60008000, 0x60400000};
    const uint64_t execute_never = UINT64_C(3) << 53;
    const uint64_t unprivileged = UINT64_C(1) << 6;
    const uint64_t attributes[] = {0xe03, 0xe03 | execute_never,
        0xe03 | execute_never | unprivileged, 0xe03 | execute_never | unprivileged, 0xe03 | execute_never};
    for (unsigned region = 0; region < EldenGmuMemoryCount; ++region) {
        for (size_t offset = 0; offset < allocations[region].bytes; offset += 4096) {
            assert(*pte(starts[region] + (uint32_t)offset) == ((allocations[region].physical + offset) | attributes[region]));
        }
    }
    assert(!*pte(0) && !*pte(0x6000c000) && !*pte(0x60407000));
    assert(((uint8_t *)tables)[context.tables.table_pages * 4096] == 0xa5);

    ArmSmmu4KIo io = {registers, SmmuBytes, read32, read64, write32, write64, barrier};
    ArmSmmu4KContextConfig config = {
        .lower = &context.translation, .lower_tables = &context.tables,
        .asid = 1, .coherent_walk = 1, .continue_after_fault = 1,
    };
    ArmSmmu4KStreamState stream = set_registers(&config);
    EldenGmuContextObservation observation;
    stream.streams[0].sid = 0;
    assert(EldenAdreno830ObserveGmuContext(&context, &io, &stream, &config, visited, sizeof(visited), &observation) == ArmSmmu4KArgument);
    stream.streams[0].sid = 5;
    assert(EldenAdreno830ObserveGmuContext(&context, &io, &stream, &config, (uint8_t *)tables, Capacity, &observation) == ArmSmmu4KOverlap);
    assert(!reads);
    assert(EldenAdreno830ObserveGmuContext(&context, &io, &stream, &config, visited, sizeof(visited), &observation) == ArmSmmu4KSuccess);
    assert(observation.streams_before.streams_verified == 1 && observation.streams_after.streams_verified == 1);
    assert(observation.tables.complete && observation.tables.lower.descriptor_reads == 13 * 512);

    registers[0x800 / 4] = 0x80000001;
    registers[0xc00 / 4] = ContextBank;
    assert(EldenAdreno830ObserveGmuContext(&context, &io, &stream, &config, visited, sizeof(visited), &observation) == ArmSmmu4KBusy);
    registers[0x800 / 4] = 0;
    registers[(ContextOffset + 0x20) / 4] ^= 0x1000;
    assert(EldenAdreno830ObserveGmuContext(&context, &io, &stream, &config, visited, sizeof(visited), &observation) == ArmSmmu4KStateMismatch);
    registers[(ContextOffset + 0x20) / 4] ^= 0x1000;
    *pte(0x60008000) ^= 0x1000;
    assert(EldenAdreno830ObserveGmuContext(&context, &io, &stream, &config, visited, sizeof(visited), &observation) == ArmSmmu4KStateMismatch);
    assert(observation.streams_before.streams_verified == 1 && !observation.tables.complete);
    *pte(0x60008000) ^= 0x1000;
    change_route_at_barrier = barriers + 4;
    assert(EldenAdreno830ObserveGmuContext(&context, &io, &stream, &config, visited, sizeof(visited), &observation) == ArmSmmu4KStateMismatch);
    assert(observation.tables.complete && !observation.streams_after.streams_verified);
    puts("Adreno 830 GMU mapping and read-only SID 5 audit passed");
}
