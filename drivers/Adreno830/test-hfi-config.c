/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "EldenAdreno830HfiConfig.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Synthetic command-db records. The frequency arrays describe the saved DT. */
#define LE16(value) ((value) & 0xff), ((value) >> 8)
static const uint8_t gfx[] = {
    LE16(0), LE16(0x40), LE16(0x80), LE16(0xc0), LE16(0x100), LE16(0x140), LE16(0x1a0), LE16(0x1c0),
};
static const uint8_t mx[] = {
    LE16(0), LE16(0x34), LE16(0x40), LE16(0x80), LE16(0x100), LE16(0x140), LE16(0x1a0), LE16(0x1c0),
};
static const uint8_t cx[] = { LE16(0), LE16(0x40), LE16(0x80), LE16(0xc0) };
static const uint8_t gmxc[] = { LE16(0), LE16(0x20), LE16(0x38), LE16(0x40), LE16(0x100), LE16(0x1c0) };
static const uint8_t bcm1[] = { 0x40, 0x42, 0x0f, 0, 1, 0, 1, 0 };
static const uint8_t bcm2[] = { 0x40, 0x42, 0x0f, 0, 1, 0, 2, 0 };
static const uint64_t gpu_hz[] = {
    160000000, 222000000, 342000000, 389000000, 443000000, 525000000, 607000000,
    660000000, 734000000, 832000000, 900000000, 967000000, 1050000000, 1100000000,
};
static const uint64_t gmu_hz[] = { 500000000, 650000000, 687500000 };

enum { Gfx, Mx, Cx, Gmxc, Sh0, Mc0, Acv, Cn0, ResourceCount };
static const char *const names[] = { "gfx.lvl", "mx.lvl", "cx.lvl", "gmxc.lvl", "SH0", "MC0", "ACV", "CN0" };
static const EldenAdrenoResource records[ResourceCount] = {
    {0x30100, gfx, sizeof(gfx)}, {0x30104, mx, sizeof(mx)}, {0x30108, cx, sizeof(cx)},
    {0x3010c, gmxc, sizeof(gmxc)}, {0x50100, bcm1, sizeof(bcm1)}, {0x50104, bcm1, sizeof(bcm1)},
    {0x50108, bcm2, sizeof(bcm2)}, {0x5010c, NULL, 0},
};
typedef struct {
    EldenAdrenoResource resources[ResourceCount];
    EldenAdrenoResourceStatus status[ResourceCount];
    unsigned lookups;
} CommandDb;

static EldenAdrenoResourceStatus lookup(void *context, const char *name, unsigned slave,
                                      EldenAdrenoResource *resource)
{
    CommandDb *db = context;
    unsigned i = db->lookups++;
    assert(i < ResourceCount && !strcmp(name, names[i]) && slave == (i <= Gmxc ? 3u : 5u));
    *resource = db->resources[i];
    return db->status[i];
}

static void reset_db(CommandDb *db)
{
    *db = (CommandDb){0};
    memcpy(db->resources, records, sizeof(records));
}

static uint64_t drm_chip_id(unsigned fuse)
{
    return (uint64_t)fuse << 32 | 0x44050000;
}

static void assert_profile(const EldenAdreno830HfiInputs *inputs, size_t bw_count)
{
    EldenAdreno830HfiConfig config;
    assert(EldenAdreno830BuildHfiConfig(inputs, &config) == EldenAdrenoHfiSuccess);
    const AdrenoHfiA8xxPerformance *perf = &config.tables.performance;
    assert(perf->gpu == config.gx && perf->gmu == config.cx);
    assert(perf->gpu_count == inputs->gpu_count + 1 && perf->gmu_count == 4);
    assert(perf->gpu[0].hz == 0 && perf->gpu[0].vote == 0 && perf->gpu[0].dependency_vote == UINT32_MAX);
    assert(perf->gpu[1].vote == 0x00400201 && perf->gpu[1].dependency_vote == 0x0fffc1ff);
    assert(perf->gpu[inputs->gpu_count].hz == gpu_hz[inputs->gpu_count - 1]);
    assert(perf->gmu[1].hz == 500000000 && perf->gmu[1].vote == 0x00400201);
    assert(perf->gmu[3].hz == 687500000 && perf->gmu[3].vote == 0x00c00403);

    uint32_t message, words[AdrenoHfiA8xxMaxPerformanceWords];
    size_t count;
    assert(AdrenoHfiEncodeA8xxPerformance(perf, &message, words, AdrenoHfiA8xxMaxPerformanceWords,
                                         &count) == AdrenoHfiSuccess);
    assert(message == 15 && count == 14 + 3 * perf->gpu_count);
    assert(words[0] == 0 && words[1] == 1 && words[2] == perf->gpu_count && words[3] == 3);
    assert(words[5] == UINT32_MAX && words[7] == 0x00400201 && words[8] == 0x0fffc1ff && words[9] == 160000);
    assert(words[4 + 3 * perf->gpu_count] == 4 && words[count - 1] == 687500);

    const AdrenoHfiBandwidth *bw = &config.tables.bandwidth;
    assert(bw->cnoc_addresses == config.bandwidth_storage.cnoc_addresses);
    assert(bw->ddr_data == config.bandwidth_storage.ddr_data);
    assert(bw->bandwidth_count == bw_count && bw->cnoc_count == 1 && bw->ddr_count == 3);
    assert(bw->cnoc_wait_mask == 1 && bw->ddr_wait_mask == 6);
    assert(bw->cnoc_addresses[0] == records[Cn0].address);
    assert(bw->cnoc_data[0][0] == 0x40000000 && bw->cnoc_data[1][0] == 0x60000001);
    assert(bw->ddr_addresses[0] == records[Sh0].address && bw->ddr_addresses[1] == records[Mc0].address &&
           bw->ddr_addresses[2] == records[Acv].address);
    assert(bw->ddr_data[0][0] == 0 && bw->ddr_data[0][1] == 0x40000000 && bw->ddr_data[0][2] == 0x40000000);
    assert(bw->ddr_data[1][0] == 0x20214085 && bw->ddr_data[1][1] == 0x60858216);
    for (size_t i = 1; i < bw_count; ++i) assert(bw->ddr_data[i][2] == 0x60000004);
    assert(bw->ddr_addresses[3] == 0 && bw->ddr_data[bw_count][2] == 0);
}

static void assert_refusal(const EldenAdreno830HfiInputs *inputs, EldenAdreno830HfiStatus status)
{
    EldenAdreno830HfiConfig output;
    unsigned char before[sizeof(output)];
    memset(&output, 0xa5, sizeof(output));
    memcpy(before, &output, sizeof(output));
    assert(EldenAdreno830BuildHfiConfig(inputs, &output) == status);
    assert(!memcmp(before, &output, sizeof(output)));
}

int main(void)
{
    CommandDb db;
    EldenAdreno830HfiInputs inputs = {
        .gpu_hz = gpu_hz, .gmu_hz = gmu_hz, .gmu_count = 3, .context = &db, .find_resource = lookup,
    };
    const struct { unsigned fuse, gpu_count, bw_count; } profiles[] = {
        {0xbe, 11, 7}, {0xdd, 13, 8}, {0xe8, 14, 8}, {0xf2, 14, 8},
    };
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
        reset_db(&db);
        inputs.drm_chip_id = drm_chip_id(profiles[i].fuse);
        inputs.gpu_count = profiles[i].gpu_count;
        assert_profile(&inputs, profiles[i].bw_count);
        assert(db.lookups == ResourceCount);
    }

    reset_db(&db);
    assert_refusal(NULL, EldenAdrenoHfiArgument);
    inputs.drm_chip_id |= 1;
    assert_refusal(&inputs, EldenAdrenoHfiIdentity);
    inputs.drm_chip_id = drm_chip_id(0xffff);
    assert_refusal(&inputs, EldenAdrenoHfiIdentity);
    inputs.drm_chip_id = drm_chip_id(0xaa);
    assert_refusal(&inputs, EldenAdrenoHfiIdentity);
    inputs.drm_chip_id = drm_chip_id(0xbe);
    assert_refusal(&inputs, EldenAdrenoHfiOppMismatch);
    inputs.gpu_count = 11;
    inputs.gmu_count = 2;
    assert_refusal(&inputs, EldenAdrenoHfiOppMismatch);
    inputs.gmu_count = 3;
    inputs.gpu_hz = gpu_hz + 1;
    assert_refusal(&inputs, EldenAdrenoHfiOppMismatch);
    inputs.gpu_hz = gpu_hz;
    assert(db.lookups == 0);

    EldenAdreno830HfiConfig config;
    db.status[Gmxc] = EldenAdrenoResourceAbsent;
    assert(EldenAdreno830BuildHfiConfig(&inputs, &config) == EldenAdrenoHfiSuccess);
    assert(config.gx[1].vote == 0x00400101 && config.gx[1].dependency_vote == 0x0fffc1ff);
    reset_db(&db);
    db.status[Gmxc] = EldenAdrenoResourceInvalid;
    assert_refusal(&inputs, EldenAdrenoHfiCommandDb);
    reset_db(&db);
    db.status[Cn0] = EldenAdrenoResourceAbsent;
    assert_refusal(&inputs, EldenAdrenoHfiCommandDb);
    reset_db(&db);
    db.resources[Gmxc].address = records[Mx].address;
    assert_refusal(&inputs, EldenAdrenoHfiCommandDb);
    reset_db(&db);
    db.resources[Sh0].address = records[Mx].address;
    assert_refusal(&inputs, EldenAdrenoHfiCommandDb);
    reset_db(&db);
    db.resources[Mc0].aux_bytes--;
    assert_refusal(&inputs, EldenAdrenoHfiCommandDb);
    reset_db(&db);
    db.resources[Mx].aux_bytes = 8;
    assert_refusal(&inputs, EldenAdrenoHfiVotes);
    reset_db(&db);
    db.resources[Gfx].aux_bytes--;
    assert_refusal(&inputs, EldenAdrenoHfiVotes);
    puts("Elden A830 HFI configuration: four profiles, votes, wire encoding and refusals passed");
    return 0;
}
