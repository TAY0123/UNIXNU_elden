/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Sm8750GpuSmmuSurvey.h"
#include "Sm8750AdrenoResources.h"

enum {
    SmmuScr0 = 0x00, SmmuSacr = 0x10, SmmuId0 = 0x20, SmmuId1 = 0x24,
    SmmuId2 = 0x28, SmmuGlobalFault = 0x48, SmmuSmr = 0x800, SmmuS2cr = 0xc00,
    SmmuCbSctlr = 0, SmmuCbActlr = 4, SmmuCbFsr = 0x58,
    SmmuStage1 = 1U << 30, SmmuStreamMatching = 1U << 27,
    SmmuFourKiB = 1U << 12, SmmuExtendedIds = 1U << 8,
    SmmuEnableExtendedIds = 1U << 3,
    SmmuExtendedValid = 1U << 10,
    SmmuRouteTypeMask = 3U << 16, SmmuRouteFault = 2U << 16,
};

int
Sm8750GpuSmmuObserve(Sm8750GpuSmmuRead read32, void *cookie, Sm8750GpuSmmuSurvey *survey)
{
    *survey = (Sm8750GpuSmmuSurvey){0};
    survey->id0 = read32(cookie, SmmuId0);
    survey->id1 = read32(cookie, SmmuId1);
    survey->id2 = read32(cookie, SmmuId2);
    survey->scr0 = read32(cookie, SmmuScr0);
    survey->sacr = read32(cookie, SmmuSacr);
    survey->global_fault = read32(cookie, SmmuGlobalFault);

    const unsigned slots = survey->id0 & 255U;
    const unsigned banks = survey->id1 & 255U;
    const unsigned stage2 = (survey->id1 >> 16) & 255U;
    const unsigned pages = 2U << ((survey->id1 >> 28) & 7U);
    const unsigned sid_bits = (survey->id0 >> 9) & 15U;
    const int extended = (survey->scr0 & SmmuEnableExtendedIds) != 0;
    if ((survey->id0 & (SmmuStage1 | SmmuStreamMatching)) != (SmmuStage1 | SmmuStreamMatching) ||
        !(survey->id2 & SmmuFourKiB) || (survey->id1 >> 31) ||
        !slots || !banks || banks > pages || stage2 > banks ||
        2U * pages * SM8750_GMU_PAGE_BYTES > SM8750_GPU_SMMU_BYTES ||
        (extended && !(survey->id0 & SmmuExtendedIds)) ||
        (!extended && sid_bits < 3U)) {
        return 0;
    }
    const uint32_t sid_mask = extended ? 0xffffU : (1U << sid_bits) - 1U;
    survey->slots = slots;
    survey->banks = banks;
    survey->stage2_banks = stage2;
    for (unsigned slot = 0; slot < slots; ++slot) {
        Sm8750GpuSmmuRoute *route = &survey->routes[slot];
        route->smr = read32(cookie, SmmuSmr + 4U * slot);
        route->s2cr = read32(cookie, SmmuS2cr + 4U * slot);
        route->valid = extended ? !!(route->s2cr & SmmuExtendedValid) : route->smr >> 31;
        const uint32_t type = route->s2cr & SmmuRouteTypeMask;
        if (!route->valid) {
            ++survey->invalid_slots;
            survey->invalid_fault_slots += type == SmmuRouteFault;
            continue;
        }
        const uint32_t ignore = route->smr >> 16;
        survey->gmu_matches += !((route->smr ^ SM8750_GMU_STREAM_ID) & sid_mask & ~ignore);
        survey->gmu_bank_routes += !type && (route->s2cr & 255U) == SM8750_GPU_GMU_SURVEY_BANK;
    }
    if (stage2 <= SM8750_GPU_GMU_SURVEY_BANK && SM8750_GPU_GMU_SURVEY_BANK < banks) {
        const uint32_t offset = (pages + SM8750_GPU_GMU_SURVEY_BANK) * SM8750_GMU_PAGE_BYTES;
        survey->gmu_bank_offset = offset;
        survey->gmu_sctlr = read32(cookie, offset + SmmuCbSctlr);
        survey->gmu_actlr = read32(cookie, offset + SmmuCbActlr);
        survey->gmu_fsr = read32(cookie, offset + SmmuCbFsr);
    }
    return 1;
}
