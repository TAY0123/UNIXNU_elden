/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AdrenoHfiA8xx.c"
/* AdrenoFirmware.c is included in the same translation unit. */
#define le32 adreno_hfi_votes_le32
#include "AdrenoHfiVotes.c"
#undef le32
#include "EldenAdreno830HfiConfig.c"

static EldenAdrenoResourceStatus
elden_adreno830_cmd_db_lookup(void *snapshot, const char *name, unsigned slave,
                            EldenAdrenoResource *out)
{
    Sm8750CmdDbResource resource;
    Sm8750CmdDbStatus status = Sm8750CmdDbRead(snapshot, Sm8750CmdDbSize, name, slave, &resource);
    if (status != Sm8750CmdDbOk) {
        return status == Sm8750CmdDbMissing ? EldenAdrenoResourceAbsent : EldenAdrenoResourceInvalid;
    }
    *out = (EldenAdrenoResource){resource.address, resource.aux, resource.aux_bytes};
    return EldenAdrenoResourceFound;
}

static void
elden_adreno830_hfi_probe(void *snapshot)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("elden-adreno830-hfi", &enabled, sizeof(enabled)) || enabled != 1) {
        return;
    }
    uint32_t speed_bin = 0;
    if (!PE_parse_boot_argn("elden-adreno830-linux-speed-bin", &speed_bin, sizeof(speed_bin))) {
        printf("ELDEN-A830: HFI requires the same-device Linux speed bin\n");
        return;
    }

    EldenAdreno830HfiConfig *config = kalloc_data(sizeof(*config), Z_WAITOK);
    if (!config) {
        printf("ELDEN-A830: HFI table allocation failed\n");
        return;
    }
    EldenAdreno830HfiStatus status = EldenAdreno830BuildHfiConfigForFuse(
        speed_bin, elden_adreno830_cmd_db_lookup, snapshot, config);
    if (status == EldenAdrenoHfiSuccess) {
        uint32_t message, payload[AdrenoHfiA8xxMaxPerformanceWords];
        size_t words;
        AdrenoHfiStatus encoded = AdrenoHfiEncodeA8xxPerformance(
            &config->tables.performance, &message, payload, AdrenoHfiA8xxMaxPerformanceWords, &words);
        if (encoded == AdrenoHfiSuccess) {
            const AdrenoHfiA8xxPerformance *perf = &config->tables.performance;
            const AdrenoHfiBandwidth *bw = &config->tables.bandwidth;
            uint32_t perf_digest = elden_adreno830_fnv1a(payload, words * sizeof(payload[0]));
            uint32_t ddr_digest = elden_adreno830_fnv1a(
                bw->ddr_data, bw->bandwidth_count * sizeof(bw->ddr_data[0]));
            printf("ELDEN-A830: HFI linux-bin=%03x gx=%lu cx=%lu ddr=%u message=%u words=%lu "
                   "perf-fnv1a=%08x ddr-fnv1a=%08x\n", speed_bin, (unsigned long)perf->gpu_count,
                   (unsigned long)perf->gmu_count, bw->bandwidth_count, message, (unsigned long)words,
                   perf_digest, ddr_digest);
        } else {
            printf("ELDEN-A830: HFI encode linux-bin=%03x status=%u\n", speed_bin, (unsigned)encoded);
        }
    } else {
        printf("ELDEN-A830: HFI inputs linux-bin=%03x status=%u\n", speed_bin, (unsigned)status);
    }
    kfree_data(config, sizeof(*config));
}
