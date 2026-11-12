/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "EldenAdreno830HfiConfig.h"
#include "AdrenoHfiVotes.h"
#include "EldenAdreno830.h"

/* Verified against the saved Elden DT and h0cheung's 6f97250 sm8750.dtsi.
 * The bandwidth is the OPP's first interconnect peak, in kB/s. */
static const struct {
    uint32_t hz, level, peak_kbps, hardware_mask;
} gpu_opps[] = {
    { 160000000, 0x032,  2136718, 0xf },
    { 222000000, 0x034,  2136718, 0xf },
    { 342000000, 0x038,  5285156, 0xf },
    { 389000000, 0x03c,  6074218, 0xf },
    { 443000000, 0x040,  6074218, 0xf },
    { 525000000, 0x050,  6074218, 0xf },
    { 607000000, 0x080,  8171875, 0xf },
    { 660000000, 0x090,  8171875, 0xf },
    { 734000000, 0x0c0, 12449218, 0xf },
    { 832000000, 0x0e0, 16500000, 0xf },
    { 900000000, 0x100, 16500000, 0xf },
    { 967000000, 0x140, 18597656, 0xe },
    {1050000000, 0x1a0, 18597656, 0xe },
    {1100000000, 0x1c0, 18597656, 0xc },
};
static const uint32_t gmu_hz[] = {500000000, 650000000, 687500000};
static const uint32_t gmu_levels[] = {0, 0x40, 0x80, 0xc0};
/* Index is the bit used by the compiled 0x44050000 catalog. */
static const uint16_t speed_fuses[] = {0xbe, 0xdd, 0xe8, 0xf2};

enum {
    GpuLevelCount = 1 + sizeof(gpu_opps) / sizeof(gpu_opps[0]),
    GmuLevelCount = sizeof(gmu_levels) / sizeof(gmu_levels[0]),
};
_Static_assert(sizeof(((EldenAdreno830HfiConfig *)0)->gx) == GpuLevelCount * sizeof(AdrenoHfiA8xxGpuLevel),
               "GX storage includes every candidate and off");
_Static_assert(sizeof(((EldenAdreno830HfiConfig *)0)->cx) == GmuLevelCount * sizeof(AdrenoHfiPerformanceLevel) &&
               GmuLevelCount == 1 + sizeof(gmu_hz) / sizeof(gmu_hz[0]),
               "CX storage includes every candidate and off");

enum { Gfx, Mx, Cx, Gmxc, Sh0, Mc0, Acv, Cn0, ResourceCount };
static const char *const resource_names[ResourceCount] = {
    "gfx.lvl", "mx.lvl", "cx.lvl", "gmxc.lvl", "SH0", "MC0", "ACV", "CN0",
};

static unsigned supported_mask(uint64_t drm_chip_id)
{
    if ((uint32_t)drm_chip_id != ELDEN_ADRENO830_CHIP_ID) return 0;
    uint64_t fuse = drm_chip_id >> 32;
    for (unsigned i = 0; i < sizeof(speed_fuses) / sizeof(speed_fuses[0]); ++i) {
        if (fuse == speed_fuses[i]) return 1u << i;
    }
    return 0;
}

static int read_resources(const EldenAdreno830HfiInputs *inputs, EldenAdrenoResource *resources)
{
    for (unsigned i = 0; i < ResourceCount; ++i) {
        unsigned slave = i <= Gmxc ? 3 : 5;
        EldenAdrenoResource *resource = &resources[i];
        EldenAdrenoResourceStatus status = inputs->find_resource(inputs->context, resource_names[i], slave, resource);
        if (i == Gmxc && status == EldenAdrenoResourceAbsent) {
            *resource = (EldenAdrenoResource){0};
            continue;
        }
        if (status != EldenAdrenoResourceFound || (resource->address & 3) ||
            (resource->address >> 16) != slave ||
            (i != Cn0 && (!resource->aux || !resource->aux_bytes))) return 0;
        for (unsigned j = 0; j < i; ++j) {
            if (resource->address == resources[j].address) return 0;
        }
    }
    return 1;
}

static uint16_t read_level(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
}

static int dependency_votes(const EldenAdrenoResource *mx, const uint32_t *levels,
                            uint32_t *votes, size_t count)
{
    /* The same-device Linux driver uses MX only and requires coverage of
     * every active GX level. The CX ARC calculation already validated MX. */
    votes[0] = UINT32_MAX;
    size_t index = 0, entries = mx->aux_bytes / 2;
    for (size_t i = 1; i < count; ++i) {
        while (index < entries && read_level(mx->aux + index * 2) < levels[i]) ++index;
        if (index == entries) return 0;
        votes[i] = (0x3fffu << 14) | (uint32_t)index << 8 | 0xff;
    }
    return 1;
}

EldenAdreno830HfiStatus EldenAdreno830BuildHfiConfig(const EldenAdreno830HfiInputs *inputs,
                                                   EldenAdreno830HfiConfig *output)
{
    if (!inputs || !output || !inputs->gpu_hz || !inputs->gmu_hz || !inputs->find_resource) {
        return EldenAdrenoHfiArgument;
    }
    unsigned mask = supported_mask(inputs->drm_chip_id);
    if (!mask) return EldenAdrenoHfiIdentity;
    if (inputs->gmu_count != sizeof(gmu_hz) / sizeof(gmu_hz[0])) return EldenAdrenoHfiOppMismatch;

    uint32_t gx_hz[GpuLevelCount] = {0}, gx_levels[GpuLevelCount] = {0};
    uint32_t gx_votes[GpuLevelCount] = {0}, gx_dependencies[GpuLevelCount] = {0}, cx_votes[GmuLevelCount];
    uint32_t bw_kbps[GpuLevelCount] = {0};
    size_t gx_count = 1, bw_count = 1;
    for (size_t i = 0; i < sizeof(gpu_opps) / sizeof(gpu_opps[0]); ++i) {
        if (!(gpu_opps[i].hardware_mask & mask)) continue;
        if (gx_count - 1 >= inputs->gpu_count || inputs->gpu_hz[gx_count - 1] != gpu_opps[i].hz) {
            return EldenAdrenoHfiOppMismatch;
        }
        gx_hz[gx_count] = gpu_opps[i].hz;
        gx_levels[gx_count++] = gpu_opps[i].level;
        if (bw_kbps[bw_count - 1] != gpu_opps[i].peak_kbps) bw_kbps[bw_count++] = gpu_opps[i].peak_kbps;
    }
    if (gx_count - 1 != inputs->gpu_count) return EldenAdrenoHfiOppMismatch;
    for (size_t i = 0; i < inputs->gmu_count; ++i) {
        if (inputs->gmu_hz[i] != gmu_hz[i]) return EldenAdrenoHfiOppMismatch;
    }

    EldenAdrenoResource resources[ResourceCount] = {{0}};
    if (!read_resources(inputs, resources)) return EldenAdrenoHfiCommandDb;
    const EldenAdrenoResource *gfx = &resources[Gfx], *mx = &resources[Mx], *cx = &resources[Cx];
    const EldenAdrenoResource *secondary = resources[Gmxc].address ? &resources[Gmxc] : mx;
    if (AdrenoHfiArcVotes(gx_levels, gx_count, gfx->aux, gfx->aux_bytes, secondary->aux,
                          secondary->aux_bytes, gx_votes, GpuLevelCount) ||
        AdrenoHfiArcVotes(gmu_levels, GmuLevelCount, cx->aux, cx->aux_bytes, mx->aux, mx->aux_bytes,
                          cx_votes, GmuLevelCount) ||
        !dependency_votes(mx, gx_levels, gx_dependencies, gx_count)) return EldenAdrenoHfiVotes;

    /* The 6f97250/bf96c3d A8xx fixed BCM uses ACV BIT(2) for every nonzero
     * bandwidth. One kB/s expresses this policy in the shared builder. */
    AdrenoHfiBcm bcms[] = {
        {.address = resources[Sh0].address, .buswidth = 16},
        {.address = resources[Mc0].address, .buswidth = 4},
        {.address = resources[Acv].address, .fixed = 1, .perfmode = 1u << 2, .perfmode_kbps = 1},
    };
    for (unsigned i = 0; i < sizeof(bcms) / sizeof(bcms[0]); ++i) {
        const EldenAdrenoResource *resource = &resources[Sh0 + i];
        if (AdrenoHfiBcmAux(&bcms[i], resource->aux, resource->aux_bytes)) return EldenAdrenoHfiCommandDb;
    }
    AdrenoHfiStartupConfig bandwidth = {0};
    if (AdrenoHfiBuildBandwidth(bw_kbps, bw_count, bcms, sizeof(bcms) / sizeof(bcms[0]),
                                 resources[Cn0].address, &bandwidth)) return EldenAdrenoHfiVotes;

    for (size_t i = 0; i < GpuLevelCount; ++i) {
        output->gx[i] = (AdrenoHfiA8xxGpuLevel){gx_hz[i], gx_votes[i], gx_dependencies[i]};
    }
    output->cx[0] = (AdrenoHfiPerformanceLevel){0, cx_votes[0]};
    for (size_t i = 1; i < GmuLevelCount; ++i) {
        output->cx[i] = (AdrenoHfiPerformanceLevel){gmu_hz[i - 1], cx_votes[i]};
    }
    output->bandwidth_storage = bandwidth;
    const AdrenoHfiStartupConfig *bw = &output->bandwidth_storage;
    output->tables.performance = (AdrenoHfiA8xxPerformance){
        AdrenoHfiA8xxPerformanceVersion, output->gx, gx_count, output->cx, GmuLevelCount,
    };
    output->tables.bandwidth = (AdrenoHfiBandwidth){
        bw->bandwidth_count, bw->cnoc_count, bw->ddr_count, bw->cnoc_wait_mask, bw->ddr_wait_mask,
        bw->cnoc_addresses, bw->cnoc_data, bw->ddr_addresses, bw->ddr_data,
    };
    return EldenAdrenoHfiSuccess;
}
