/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "EldenAdreno830GmuBoot.h"
#include "AdrenoHfiA8xx.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* GMU-relative byte offsets from the paired Linux register definitions.
 * Keep them independent of the production register macros. */
enum {
    Itcm = 0x36000, Dtcm = 0x3a000, Version = 0x3dfe0,
    Reset = 0x47000, BootConfig = 0x47004, FirmwareInit = 0x47070,
    HfiStatus = 0x47600, HfiTable = 0x47614, HfiInit = 0x47618,
    GmuToHost = 0x47648, Doorbell = 0x47650, LogConfig = 0x47720,
    LogIndex = 0x47724, GmuChip = 0x47728, AoIrq = 0x57c14,
    FenceRange = 0x57c44,
};

typedef enum {
    Ready, StaleFirmware, StaleHfi, NoFirmware, NoHfi,
    FirmwareFault, Watchdog, PlatformFault, WaitFailed, ClockStopped,
    GpuWriteFailed, ReleaseWriteFailed, ResetWriteFailed,
} Scenario;

static _Alignas(4096) uint8_t image[88896], icache[0x16000];
static _Alignas(4096) uint8_t gmu_log[0x4000];
static _Alignas(4096) uint32_t hfi_words[4096];
static struct {
    uint32_t registers[0x68000 / 4];
    uint64_t now;
    unsigned writes, syncs, barriers, gbif_writes, releases, waits, bells;
    unsigned firmware_waits, hfi_waits;
    Scenario scenario;
} model;
static EldenGmuBoot boot;

static int read32(void *context, uint32_t offset, uint32_t *value)
{
    assert(context == &model && !(offset & 3) && offset < sizeof(model.registers));
    *value = model.registers[offset / 4];
    return 0;
}

static int write32(void *context, uint32_t offset, uint32_t value)
{
    assert(context == &model && !(offset & 3) && offset < sizeof(model.registers));
    ++model.writes;
    if (offset == FirmwareInit && model.scenario == StaleFirmware) return 0;
    if (offset == Reset && value == 1 && model.scenario == ResetWriteFailed) return -1;
    if (offset == Reset && value == 0) {
        assert(model.registers[Reset / 4] == 1 && model.syncs == 1);
        assert(model.gbif_writes == 5 && model.barriers == 2);
        assert(model.registers[BootConfig / 4] == 2);
        assert(model.registers[FirmwareInit / 4] == 0 && model.registers[HfiStatus / 4] == 0);
        assert(model.registers[HfiTable / 4] == 0x60008000 && hfi_words[19] == 0x6000a000);
        assert(model.registers[LogConfig / 4] == 0x60004003 && model.registers[LogIndex / 4] == 0);
        assert(model.registers[GmuChip / 4] == 0x08000100);
        assert(model.registers[FenceRange / 4] == 0x80c808c0);
        assert(!memcmp(&model.registers[Itcm / 4], image + 0xc0, 0x1ec));
        assert(!memcmp(&model.registers[Dtcm / 4 + 0x3fdc / 4], image + 0x15b28, 0x18));
        assert(!memcmp(icache, image + 0x2bc, 0x15824));
        ++model.releases;
        if (model.scenario == ReleaseWriteFailed) {
            model.registers[Reset / 4] = 0;
            return -1;
        }
    }
    if (offset == HfiInit) {
        assert(model.registers[FirmwareInit / 4] == 0x100);
        assert(model.registers[Reset / 4] == 0 && value == 1);
    }
    if (offset == Doorbell) {
        assert(boot.stage == EldenGmuBootRunning && value == 1);
        assert(model.registers[HfiStatus / 4] == 1 && model.bells < 2);
        uint32_t command = hfi_words[1024 + hfi_words[16]];
        assert((command & 0xff) == (model.bells ? 3u : 15u));
        assert((command >> 20) == ++model.bells);
        hfi_words[16] = hfi_words[17];
        uint32_t cursor = hfi_words[29];
        hfi_words[2048 + cursor] = 0x1037e;
        hfi_words[2049 + cursor] = command;
        hfi_words[2050 + cursor] = 0;
        hfi_words[29] = cursor + 4;
    }
    model.registers[offset / 4] = value;
    return 0;
}

static int write_gpu32(void *context, uint32_t offset, uint32_t value)
{
    assert(context == &model && model.registers[Reset / 4] == 1);
    static const uint32_t offsets[] = {0xf00c, 0xf010, 0xf014, 0xf018, 0xf000};
    unsigned i = model.gbif_writes++;
    assert(i < sizeof(offsets) / sizeof(offsets[0]) && offset == offsets[i]);
    assert(value == (i == 4 ? 0x20023000 : 0x00071e20));
    return model.scenario == GpuWriteFailed ? -1 : 0;
}

static int sync_for_device(void *context, void *cpu, size_t bytes)
{
    assert(context == &model && model.registers[Reset / 4] == 1);
    assert(cpu == icache && bytes == 0x15824);
    ++model.syncs;
    return 0;
}

static int barrier(void *context)
{
    assert(context == &model);
    ++model.barriers;
    return 0;
}

static int health(void *context)
{
    assert(context == &model);
    return model.releases && model.scenario == PlatformFault ? -1 : 0;
}

static int now_us(void *context, uint64_t *value)
{
    assert(context == &model);
    *value = model.now;
    return 0;
}

static int wait_us(void *context, unsigned delay)
{
    assert(context == &model && delay == 100 && model.registers[Reset / 4] == 0);
    ++model.waits;
    if (model.scenario == WaitFailed) return -1;
    if (model.scenario != ClockStopped) model.now += delay;
    if (model.registers[HfiInit / 4]) {
        if (++model.hfi_waits == 2 && model.scenario != NoHfi) {
            model.registers[HfiStatus / 4] = 1;
        }
    } else if (++model.firmware_waits == 2) {
        switch (model.scenario) {
        case FirmwareFault:
            model.registers[GmuToHost / 4] |= 1u << 23;
            break;
        case Watchdog:
            model.registers[AoIrq / 4] |= 1;
            break;
        case NoFirmware:
        case ClockStopped:
        case ResetWriteFailed:
            break;
        default:
            model.registers[FirmwareInit / 4] = 0x100;
        }
    }
    return 0;
}

static const EldenGmuBootServices services = {
    .transfer_io = {
        .context = &model, .mmio_bytes = 0x68000, .read32 = read32,
        .write32 = write32, .sync_for_device = sync_for_device, .barrier = barrier,
    },
    .write_gpu32 = write_gpu32, .check_health = health, .now_us = now_us, .wait_us = wait_us,
};
static const EldenGmuContext memory = {
    .buffers = {
        [EldenGmuMemoryIcache] = {icache, sizeof(icache), 0x4000},
        [EldenGmuMemoryLog] = {gmu_log, sizeof(gmu_log), 0x60004000},
        [EldenGmuMemoryHfi] = {(uint8_t *)hfi_words, sizeof(hfi_words), 0x60008000},
    },
};

static void setup(Scenario scenario)
{
    memset(&model, 0, sizeof(model));
    memset(hfi_words, 0xa5, sizeof(hfi_words));
    boot = (EldenGmuBoot){0};
    model.scenario = scenario;
    model.registers[Reset / 4] = 1;
    model.registers[FirmwareInit / 4] = 0x100;
    model.registers[HfiStatus / 4] = scenario == StaleHfi;
    model.registers[GmuToHost / 4] = (1u << 26) | 1u;
}

static EldenGmuBootResult run(size_t bytes)
{
    return EldenAdreno830BootGmu(&boot, 0x44050000, image, bytes, &memory, &services);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file && fread(image, 1, sizeof(image), file) == sizeof(image));
    assert(fgetc(file) == EOF && !ferror(file));
    assert(!fclose(file));

    setup(Ready);
    assert(run(sizeof(image)) == EldenGmuBootSuccess && boot.stage == EldenGmuBootRunning);
    assert(boot.core_version == 0x50010019 && model.registers[Version / 4] == boot.core_version);
    assert(model.releases == 1 && model.waits == 4);
    assert(model.registers[GmuToHost / 4] == ((1u << 26) | 1u));
    assert(run(sizeof(image)) == EldenGmuBootAlreadyAttempted && model.releases == 1);
    static const AdrenoHfiA8xxGpuLevel gx[] = {{0, 0, UINT32_MAX}, {160000000, 1, 2}};
    static const AdrenoHfiPerformanceLevel cx[] = {{0, 0}, {500000000, 3}};
    static const uint32_t cnoc_addresses[6] = {0x1000}, ddr_addresses[8] = {0x2000, 0x2004};
    static const uint32_t cnoc_data[2][6] = {{0x40000000}, {0x60000001}};
    static const uint32_t ddr_data[16][8] = {{0, 0x40000000}, {0x20000002, 0x60000003}};
    AdrenoHfiA8xxTablesConfig tables = {
        .performance = {0, gx, 2, cx, 2},
        .bandwidth = {2, 1, 2, 1, 2, cnoc_addresses, cnoc_data, ddr_addresses, ddr_data},
    };
    AdrenoHfiStartupServices hfi_services = {&model, now_us, wait_us, health, 3, 100, 300, 600};
    AdrenoHfiA8xxTablesState table_state = {0};
    AdrenoHfiStartupResult table_result;
    assert(AdrenoHfiA8xxProgramTables(&table_state, &boot.hfi, &tables, &hfi_services,
        &table_result) == AdrenoHfiStartupSuccess && model.bells == 2);
    assert(table_result.acknowledged_stages ==
        ((1u << AdrenoHfiStagePerformance) | (1u << AdrenoHfiStageBandwidth)));

    setup(Ready);
    assert(run(sizeof(image) - 1) == EldenGmuBootTransferFailed);
    assert(boot.transfer_result == EldenGmuInvalidImage && !model.writes);

    static const struct {
        Scenario scenario;
        EldenGmuBootResult result;
        EldenGmuBootStage stage;
        uint32_t reg;
    } failures[] = {
        {StaleFirmware, EldenGmuBootUnexpectedState, EldenGmuBootConfigure, FirmwareInit},
        {StaleHfi, EldenGmuBootUnexpectedState, EldenGmuBootConfigure, HfiStatus},
        {GpuWriteFailed, EldenGmuBootIoFailed, EldenGmuBootConfigure, 0xf00c},
        {NoFirmware, EldenGmuBootTimeout, EldenGmuBootFirmwareReady, FirmwareInit},
        {NoHfi, EldenGmuBootTimeout, EldenGmuBootHfiReady, HfiStatus},
        {FirmwareFault, EldenGmuBootUnhealthy, EldenGmuBootFirmwareReady, GmuToHost},
        {Watchdog, EldenGmuBootUnhealthy, EldenGmuBootFirmwareReady, AoIrq},
        {PlatformFault, EldenGmuBootUnhealthy, EldenGmuBootFirmwareReady, Reset},
        {WaitFailed, EldenGmuBootIoFailed, EldenGmuBootFirmwareReady, FirmwareInit},
        {ClockStopped, EldenGmuBootTimeout, EldenGmuBootFirmwareReady, FirmwareInit},
        {ReleaseWriteFailed, EldenGmuBootIoFailed, EldenGmuBootFirmwareReady, Reset},
        {ResetWriteFailed, EldenGmuBootTimeout, EldenGmuBootFirmwareReady, FirmwareInit},
    };
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        setup(failures[i].scenario);
        assert(run(sizeof(image)) == failures[i].result);
        assert(boot.stage == failures[i].stage && boot.reg == failures[i].reg);
        assert(boot.gpu_register == (failures[i].scenario == GpuWriteFailed));
        assert(!boot.hfi.shared && !model.bells && model.waits <= 102);
        assert(boot.reset_reasserted == (model.releases && failures[i].scenario != ResetWriteFailed));
        assert(model.registers[Reset / 4] == (failures[i].scenario != ResetWriteFailed));
        assert(run(sizeof(image)) == EldenGmuBootAlreadyAttempted);
    }
    puts("Elden Adreno 830 cold boot and HFI register model passed");
}
