/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "EldenAdreno830GmuBoot.h"
#include "Sm8750AdrenoResources.h"

enum {
    GmuPollIntervalUs = 100,
    GmuReadyTimeoutUs = 10000,
    GmuHfiReadyBit = 1,
};

static int
read_gmu(EldenGmuBoot *boot, const EldenGmuTransfer *io, uint32_t reg)
{
    boot->gpu_register = 0;
    boot->reg = reg;
    boot->value = 0;
    return io->read32(io->context, reg, &boot->value);
}

static int
write_gmu(EldenGmuBoot *boot, const EldenGmuTransfer *io, uint32_t reg, uint32_t value)
{
    boot->gpu_register = 0;
    boot->reg = reg;
    boot->value = value;
    return io->write32(io->context, reg, value);
}

static EldenGmuBootResult
check_health(EldenGmuBoot *boot, const EldenGmuBootServices *services)
{
    const EldenGmuTransfer *io = &services->transfer_io;
    if (services->check_health(io->context)) return EldenGmuBootUnhealthy;

    if (read_gmu(boot, io, SM8750_ADRENO_GMU_GMU2HOST)) return EldenGmuBootIoFailed;
    if (boot->value & SM8750_ADRENO_GMU_CM3_FAULT) return EldenGmuBootUnhealthy;

    if (read_gmu(boot, io, SM8750_ADRENO_GMU_AO_IRQ)) return EldenGmuBootIoFailed;
    if (boot->value & SM8750_ADRENO_GMU_AO_FAULTS) return EldenGmuBootUnhealthy;

    return EldenGmuBootSuccess;
}

static EldenGmuBootResult
poll_ready(EldenGmuBoot *boot, const EldenGmuBootServices *services,
    uint32_t reg, uint32_t mask, uint32_t expected)
{
    const EldenGmuTransfer *io = &services->transfer_io;
    uint64_t start;
    if (services->now_us(io->context, &start)) return EldenGmuBootIoFailed;
    uint64_t previous = start;

    for (unsigned attempt = 0; attempt <= GmuReadyTimeoutUs / GmuPollIntervalUs; ++attempt) {
        EldenGmuBootResult health = check_health(boot, services);
        if (health != EldenGmuBootSuccess) return health;
        if (read_gmu(boot, io, reg)) return EldenGmuBootIoFailed;

        uint64_t now;
        if (services->now_us(io->context, &now) || now < previous) {
            return EldenGmuBootIoFailed;
        }
        if (now - start > GmuReadyTimeoutUs) return EldenGmuBootTimeout;
        if ((boot->value & mask) == expected) return EldenGmuBootSuccess;
        if (now - start == GmuReadyTimeoutUs ||
            attempt == GmuReadyTimeoutUs / GmuPollIntervalUs) {
            return EldenGmuBootTimeout;
        }
        if (services->wait_us(io->context, GmuPollIntervalUs)) return EldenGmuBootIoFailed;
        previous = now;
    }
    return EldenGmuBootTimeout;
}

static int
ring_doorbell(void *context)
{
    const EldenGmuBootServices *services = context;
    const EldenGmuTransfer *io = &services->transfer_io;
    return io->write32(io->context, SM8750_ADRENO_GMU_HOST2GMU, 1);
}

static EldenGmuBootResult
configure_gmu(EldenGmuBoot *boot, const EldenGmuBootServices *services,
    const EldenGmuBuffer *hfi, const EldenGmuBuffer *log)
{
    const EldenGmuTransfer *io = &services->transfer_io;
    const struct {
        uint32_t reg;
        uint32_t value;
    } boot_registers[] = {
        {SM8750_ADRENO_GMU_FW_INIT, 0},
        {SM8750_ADRENO_GMU_BOOT_CONFIG, 2},
        {SM8750_ADRENO_GMU_HFI_TABLE, (uint32_t)hfi->iova},
        {SM8750_ADRENO_GMU_HFI_INFO, 1},
        {SM8750_ADRENO_GMU_FENCE_RANGE, (1U << 31) | (0x32U << 18) | 0x8c0U},
        {SM8750_ADRENO_GMU_CM3_CONFIG, 0x4052},
        {SM8750_ADRENO_GMU_CHIP_ID, 0x08000100},
        {SM8750_ADRENO_GMU_LOG_CONFIG, (uint32_t)log->iova |
                                     ((uint32_t)log->bytes / SM8750_GMU_PAGE_BYTES - 1)},
        {SM8750_ADRENO_GMU_LOG_INDEX, 0},
    }, gmu_bus_registers[] = {
        {SM8750_ADRENO_GMU_GBIF_QOS, 0x33},
        {SM8750_ADRENO_GMU_SYS_BUS_CONFIG, 1},
        {SM8750_ADRENO_GMU_ICACHE_CONFIG, 1},
        {SM8750_ADRENO_GMU_DCACHE_CONFIG, 1},
    }, gbif_registers[] = {
        {SM8750_ADRENO_GBIF_QSB_SIDE0, 0x00071e20},
        {SM8750_ADRENO_GBIF_QSB_SIDE1, 0x00071e20},
        {SM8750_ADRENO_GBIF_QSB_SIDE2, 0x00071e20},
        {SM8750_ADRENO_GBIF_QSB_SIDE3, 0x00071e20},
        {SM8750_ADRENO_GBIF_CX_CONFIG, 0x20023000},
    };

    for (size_t i = 0; i < sizeof(boot_registers) / sizeof(boot_registers[0]); ++i) {
        if (write_gmu(boot, io, boot_registers[i].reg, boot_registers[i].value)) {
            return EldenGmuBootIoFailed;
        }
    }
    boot->gpu_register = 1;
    for (size_t i = 0; i < sizeof(gbif_registers) / sizeof(gbif_registers[0]); ++i) {
        boot->reg = gbif_registers[i].reg;
        boot->value = gbif_registers[i].value;
        if (services->write_gpu32(io->context, boot->reg, boot->value)) {
            return EldenGmuBootIoFailed;
        }
    }
    for (size_t i = 0; i < sizeof(gmu_bus_registers) / sizeof(gmu_bus_registers[0]); ++i) {
        if (write_gmu(boot, io, gmu_bus_registers[i].reg, gmu_bus_registers[i].value)) {
            return EldenGmuBootIoFailed;
        }
    }
    if (io->barrier(io->context) || read_gmu(boot, io, SM8750_ADRENO_GMU_FW_INIT)) {
        return EldenGmuBootIoFailed;
    }
    if (boot->value) return EldenGmuBootUnexpectedState;

    if (read_gmu(boot, io, SM8750_ADRENO_GMU_HFI_STATUS)) return EldenGmuBootIoFailed;
    if (boot->value & GmuHfiReadyBit) return EldenGmuBootUnexpectedState;

    return check_health(boot, services);
}

static EldenGmuBootResult
start_gmu(EldenGmuBoot *boot, const EldenGmuBootServices *services)
{
    const EldenGmuTransfer *io = &services->transfer_io;
    if (write_gmu(boot, io, SM8750_ADRENO_GMU_CM3_SYSRESET, 0) ||
        io->barrier(io->context) ||
        read_gmu(boot, io, SM8750_ADRENO_GMU_CM3_SYSRESET)) {
        return EldenGmuBootIoFailed;
    }
    if (boot->value) return EldenGmuBootUnexpectedState;

    const int legacy = boot->core_version <= 0x20010004U;
    EldenGmuBootResult result = poll_ready(boot, services, SM8750_ADRENO_GMU_FW_INIT,
        legacy ? UINT32_MAX : 0x1ffU, legacy ? 0xbabefaceU : 0x100U);
    if (result != EldenGmuBootSuccess) return result;

    boot->stage = EldenGmuBootHfiReady;
    if (read_gmu(boot, io, SM8750_ADRENO_GMU_HFI_STATUS)) return EldenGmuBootIoFailed;
    if (boot->value & GmuHfiReadyBit) return EldenGmuBootUnexpectedState;
    if (write_gmu(boot, io, SM8750_ADRENO_GMU_HFI_INIT, 1) || io->barrier(io->context)) {
        return EldenGmuBootIoFailed;
    }
    return poll_ready(boot, services, SM8750_ADRENO_GMU_HFI_STATUS,
        GmuHfiReadyBit, GmuHfiReadyBit);
}

EldenGmuBootResult
EldenAdreno830BootGmu(EldenGmuBoot *boot, uint32_t chip_id, const uint8_t *image, size_t bytes,
    const EldenGmuContext *memory, const EldenGmuBootServices *services)
{
    if (!boot || !image || !bytes || !memory || !services || !services->write_gpu32 ||
        !services->check_health || !services->now_us || !services->wait_us) {
        return EldenGmuBootInvalid;
    }
    if (boot->stage != EldenGmuBootIdle) return EldenGmuBootAlreadyAttempted;

    EldenGmuTransfer io = services->transfer_io;
    if (io.mmio_bytes != SM8750_ADRENO_GMU_BYTES || !io.read32 || !io.write32 ||
        !io.sync_for_device || !io.barrier || chip_id != ELDEN_ADRENO830_CHIP_ID) {
        return EldenGmuBootInvalid;
    }
    const EldenGmuBuffer *hfi = &memory->buffers[EldenGmuMemoryHfi];
    const EldenGmuBuffer *log = &memory->buffers[EldenGmuMemoryLog];
    if (hfi->iova != SM8750_GMU_HFI_IOVA || hfi->bytes != SM8750_GMU_HFI_BYTES ||
        !hfi->cpu || ((uintptr_t)hfi->cpu & (SM8750_GMU_PAGE_BYTES - 1)) ||
        (uintptr_t)hfi->cpu > UINTPTR_MAX - hfi->bytes ||
        log->iova != SM8750_GMU_LOG_IOVA || log->bytes != SM8750_GMU_LOG_BYTES ||
        !log->cpu || ((uintptr_t)log->cpu & (SM8750_GMU_PAGE_BYTES - 1)) ||
        (uintptr_t)log->cpu > UINTPTR_MAX - log->bytes) {
        return EldenGmuBootInvalid;
    }
    io.icache = memory->buffers[EldenGmuMemoryIcache];
    io.scratch = memory->buffers[EldenGmuMemoryScratch];

    boot->stage = EldenGmuBootTransfer;
    EldenGmuBootResult health = check_health(boot, services);
    if (health != EldenGmuBootSuccess) return health;
    if (read_gmu(boot, &io, SM8750_ADRENO_GMU_CM3_SYSRESET)) return EldenGmuBootIoFailed;
    if (boot->value != 1) return EldenGmuBootUnexpectedState;

    boot->transfer_result = EldenAdreno830TransferGmu(chip_id, image, bytes, &io);
    if (boot->transfer_result != EldenGmuSuccess) return EldenGmuBootTransferFailed;

    boot->stage = EldenGmuBootConfigure;
    if (read_gmu(boot, &io, SM8750_ADRENO_GMU_CORE_VERSION)) return EldenGmuBootIoFailed;
    boot->core_version = boot->value;
    if (!boot->core_version || boot->core_version == UINT32_MAX) {
        return EldenGmuBootUnexpectedState;
    }
    AdrenoHfi prepared;
    if (AdrenoHfiInitializeStopped(&prepared, (volatile uint32_t *)hfi->cpu,
        hfi->bytes, hfi->iova, (void *)services, ring_doorbell) != AdrenoHfiSuccess) {
        return EldenGmuBootInvalid;
    }

    EldenGmuBootResult result = configure_gmu(boot, services, hfi, log);
    if (result != EldenGmuBootSuccess) return result;
    if (read_gmu(boot, &io, SM8750_ADRENO_GMU_CM3_SYSRESET)) return EldenGmuBootIoFailed;
    if (boot->value != 1) return EldenGmuBootUnexpectedState;

    boot->stage = EldenGmuBootFirmwareReady;
    result = start_gmu(boot, services);
    if (result == EldenGmuBootSuccess) {
        boot->hfi = prepared;
        boot->stage = EldenGmuBootRunning;
        return result;
    }

    uint32_t reset;
    if (!io.write32(io.context, SM8750_ADRENO_GMU_CM3_SYSRESET, 1) &&
        !io.barrier(io.context) &&
        !io.read32(io.context, SM8750_ADRENO_GMU_CM3_SYSRESET, &reset) && reset == 1) {
        boot->reset_reasserted = 1;
    }
    return result;
}
