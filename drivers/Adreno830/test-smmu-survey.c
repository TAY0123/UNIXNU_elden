/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Sm8750AdrenoResources.h"
#include "Sm8750GpuSmmuSurvey.h"
#include <assert.h>
#include <stdio.h>

static uint32_t registers[SM8750_GPU_SMMU_BYTES / 4U];
static unsigned reads;

static uint32_t
read_register(void *cookie, uint32_t offset)
{
    const uint32_t *window = cookie;
    assert(offset < SM8750_GPU_SMMU_BYTES && !(offset & 3U));
    ++reads;
    return window[offset / 4U];
}

int
main(void)
{
    Sm8750GpuSmmuSurvey survey;
    /* SM8750 handoff shape from the PKX110 reference. Invalid SMRs
     * make the inherited S2CR values inactive. */
    registers[0x00 / 4] = 0x002e0406;
    registers[0x20 / 4] = 0x4c017e18;
    registers[0x24 / 4] = 0x40000015;
    registers[0x28 / 4] = 0x00005511;
    for (unsigned slot = 0; slot < 24; ++slot) {
        registers[0xc00 / 4 + slot] = 0x03000018;
    }
    registers[0x22004 / 4] = 0x303;
    registers[0x22058 / 4] = 0x400;
    assert(Sm8750GpuSmmuObserve(read_register, registers, &survey));
    assert(reads == 6 + 2 * 24 + 3);
    assert(survey.slots == 24 && survey.banks == 21 && survey.stage2_banks == 0);
    assert(survey.invalid_slots == 24 && survey.invalid_fault_slots == 0);
    assert(!survey.gmu_matches && !survey.gmu_bank_routes);
    assert(survey.gmu_bank_offset == 0x22000 && survey.gmu_actlr == 0x303 && survey.gmu_fsr == 0x400);

    registers[0x800 / 4 + 1] = 0x80010004; /* mask bit 0 makes SID 4 cover SID 5 */
    registers[0xc00 / 4 + 1] = 2;          /* translated to CB2 */
    registers[0x800 / 4 + 2] = 0x80000005;
    registers[0xc00 / 4 + 2] = 0x00020000; /* exact SID 5 in fault state */
    registers[0xc00 / 4 + 3] = 0x00020000; /* invalid slot in fault state */
    assert(Sm8750GpuSmmuObserve(read_register, registers, &survey));
    assert(survey.gmu_matches == 2 && survey.gmu_bank_routes == 1);
    assert(survey.invalid_slots == 22 && survey.invalid_fault_slots == 1);

    registers[0x00 / 4] |= 8;
    assert(!Sm8750GpuSmmuObserve(read_register, registers, &survey));
    registers[0x20 / 4] |= 0x100;
    registers[0xc00 / 4 + 1] = 0x402;
    assert(Sm8750GpuSmmuObserve(read_register, registers, &survey));
    assert(survey.routes[1].valid && !survey.routes[2].valid);
    assert(survey.gmu_matches == 1 && survey.gmu_bank_routes == 1);

    registers[0x24 / 4] = 0x40030015;
    assert(Sm8750GpuSmmuObserve(read_register, registers, &survey));
    assert(!survey.gmu_bank_offset);
    registers[0x24 / 4] = 0x70000015; /* context pages would exceed the mapping */
    reads = 0;
    assert(!Sm8750GpuSmmuObserve(read_register, registers, &survey));
    assert(reads == 6 && !survey.slots && !survey.gmu_bank_offset);
    puts("SM8750 GPU SMMU survey passed");
    return 0;
}
