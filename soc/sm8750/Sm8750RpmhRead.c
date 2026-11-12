/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Sm8750RpmhRead.h"
#include "Sm8750AdrenoResources.h"
#include "Sm8750CmdDb.h"
#include <stdbool.h>

/* Qualcomm RSC v3, as reported by the passive Elden readback. */
enum {
    RscId = 0,
    RscConfig = 0x0c,
    EldenRscId = 0x00030800,
    SlotBit = 1u << 1,
    IrqEnable = SM8750_APPS_RSC_TCS_BASE,
    IrqStatus = IrqEnable + 4,
    IrqClear = IrqEnable + 8,
    Tcs1 = SM8750_APPS_RSC_TCS_BASE + SM8750_APPS_RSC_TCS_STRIDE,
    TcsWait = Tcs1 + 0x20,
    TcsControl = Tcs1 + 0x24,
    TcsStatus = Tcs1 + 0x28,
    TcsEnable = Tcs1 + 0x2c,
    CmdMessage = Tcs1 + 0x34,
    CmdAddress = Tcs1 + 0x38,
    CmdData = Tcs1 + 0x3c,
    CmdStatus = Tcs1 + 0x40,
    CmdResponse = Tcs1 + 0x44,
    ReadResponse = 8 | (1u << 8),
    WriteResponse = ReadResponse | (1u << 16),
    CommandComplete = (1u << 8) | (1u << 16),
    AmcEnable = 1u << 16,
    AmcTrigger = 1u << 24,
};

static uint32_t
read_register(const Sm8750RpmhReadIo *io, uint32_t reg)
{
    return io->read(io->context, reg);
}

static bool
wait_register(const Sm8750RpmhReadIo *io, Sm8750RpmhReadResult *result,
              uint32_t reg, uint32_t mask, uint32_t value)
{
    for (unsigned retry = 0; retry < 100; ++retry) {
        if ((read_register(io, reg) & mask) == value) {
            return true;
        }
        io->delay(io->context, 1);
    }
    result->failed_register = reg;
    return false;
}

static bool
write_checked(const Sm8750RpmhReadIo *io, Sm8750RpmhReadResult *result,
              uint32_t reg, uint32_t value)
{
    io->write(io->context, reg, value);
    return wait_register(io, result, reg, UINT32_MAX, value);
}

static bool
rpmh_io_valid(const Sm8750RpmhReadIo *io)
{
    return io && io->read && io->write && io->delay && io->triggering;
}

static Sm8750RpmhReadStatus
transact_arc(const Sm8750RpmhReadIo *io, uint32_t address, const uint32_t *vote,
             Sm8750RpmhReadResult *result)
{
    *result = (Sm8750RpmhReadResult){0};
    result->hardware = read_register(io, RscId);
    result->config = read_register(io, RscConfig);
    if (result->hardware != EldenRscId || result->config != UINT32_C(0x800c8104)) {
        return Sm8750RpmhReadHardware;
    }
    result->irq_status = read_register(io, IrqStatus);
    if (((read_register(io, IrqEnable) | result->irq_status) & SlotBit) ||
        read_register(io, TcsWait) || read_register(io, TcsControl) ||
        read_register(io, TcsStatus) != 1 || read_register(io, TcsEnable)) {
        return Sm8750RpmhReadBusy;
    }
    uint32_t message = read_register(io, CmdMessage);
    uint32_t target = read_register(io, CmdAddress);
    uint32_t data = vote ? read_register(io, CmdData) : 0;
    bool programmed = write_checked(io, result, CmdMessage, vote ? WriteResponse : ReadResponse) &&
        write_checked(io, result, CmdAddress, address) &&
        (!vote || (write_checked(io, result, CmdData, *vote) &&
                   write_checked(io, result, TcsWait, 1))) &&
        write_checked(io, result, TcsEnable, 1) &&
        write_checked(io, result, TcsControl, AmcEnable);
    Sm8750RpmhReadStatus status = Sm8750RpmhReadProgramming;
    if (programmed) {
        io->triggering(io->context);
        result->triggered = 1;
        io->write(io->context, TcsControl, AmcEnable | AmcTrigger);
        unsigned polls;
        for (polls = 0; polls < 1000; ++polls) {
            io->delay(io->context, 100);
            result->irq_status = read_register(io, IrqStatus);
            if ((result->irq_status & SlotBit) && read_register(io, TcsStatus) == 1) {
                break;
            }
        }
        result->command_status = read_register(io, CmdStatus);
        if (polls == 1000) {
            return Sm8750RpmhReadTimeout;
        }
        if (result->command_status != UINT32_MAX &&
            (result->command_status & CommandComplete) == CommandComplete) {
            status = Sm8750RpmhReadOk;
            result->acknowledged = 1;
            if (!vote) result->response = read_register(io, CmdResponse);
        } else {
            status = Sm8750RpmhReadIncomplete;
        }
    }
    if ((programmed && !write_checked(io, result, TcsControl, AmcEnable)) ||
        !write_checked(io, result, TcsControl, 0) ||
        !write_checked(io, result, TcsEnable, 0) ||
        (vote && (!write_checked(io, result, TcsWait, 0) ||
                  !write_checked(io, result, CmdData, data))) ||
        !write_checked(io, result, CmdMessage, message) ||
        !write_checked(io, result, CmdAddress, target)) {
        return Sm8750RpmhReadCleanup;
    }
    if (programmed) {
        io->write(io->context, IrqClear, SlotBit);
        if (!wait_register(io, result, IrqStatus, SlotBit, 0)) {
            return Sm8750RpmhReadCleanup;
        }
    }
    return status;
}

Sm8750RpmhReadStatus
Sm8750RpmhReadArc(const Sm8750RpmhReadIo *io, uint32_t address, Sm8750RpmhReadResult *result)
{
    if (!result || !rpmh_io_valid(io) || (address & 3) || address >> 16 != Sm8750CmdDbArc) {
        return Sm8750RpmhReadArgument;
    }
    return transact_arc(io, address, NULL, result);
}

Sm8750RpmhReadStatus
Sm8750RpmhHoldGpuRails(const Sm8750RpmhReadIo *io, const void *cmd_db, size_t bytes,
                      Sm8750RpmhGpuRailResult *result)
{
    if (!result || !rpmh_io_valid(io)) return Sm8750RpmhReadArgument;
    *result = (Sm8750RpmhGpuRailResult){0};

    /* Before AP vote aggregation is available, use each rail's highest
     * supported corner. A LOW_SVS write could replace a higher AP vote. */
    enum { LowSvs = 0x40, EldenMxAddress = 0x30010, EldenCxAddress = 0x30000 };
    uint32_t mx_address, cx_address;
    unsigned mx_level, cx_level;
    if (Sm8750CmdDbArcTop(cmd_db, bytes, "mx.lvl", &mx_address, &result->mx_index, &mx_level) ||
        Sm8750CmdDbArcTop(cmd_db, bytes, "cx.lvl", &cx_address, &result->cx_index, &cx_level) ||
        mx_address != EldenMxAddress || cx_address != EldenCxAddress ||
        mx_level < LowSvs || cx_level < LowSvs) {
        return Sm8750RpmhReadArgument;
    }
    uint32_t mx_vote = result->mx_index, cx_vote = result->cx_index;
    Sm8750RpmhReadStatus status = transact_arc(io, mx_address, &mx_vote, &result->mx);
    if (status != Sm8750RpmhReadOk) return status;
    return transact_arc(io, cx_address, &cx_vote, &result->cx);
}
