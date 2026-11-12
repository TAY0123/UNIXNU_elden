/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 NativeAdreno contributors */
/* SM8750 power-up sequence adapted from Adreno830's PKX110 port. */
#include "Sm8750GpuPower.h"
#include "Sm8750AdrenoResources.h"
#include <stdbool.h>
#include <stddef.h>

enum {
    GccClockVote = SM8750_GCC_GPU_VOTE_REG, GccCfgAhb = 0x71004, GccGpuMemnoc = 0x71010,
    GccDdrGpuAxi = 0x71150, GpllVotes = (1u << 15) | (1u << 16),
    CxGdsc = SM8750_GPUCC_CX_PAGE_OFFSET + SM8750_GPUCC_CX_GDSCR,
    CxHardware = SM8750_GPUCC_CX_PAGE_OFFSET + SM8750_GPUCC_CX_GDS_HW_CTRL,
    CxBcr = 0x907c, GmuBcr = 0x9314,
    GmuRoot = 0x9318, HubRoot = 0x93ec,
    PllMode = 0, PllOpmode = 4, PllL = 0x10, PllAlpha = 0x14,
    PllUser = 0x18, PllUserHi = 0x1c, PllConfig = 0x20,
    PllConfigHi = 0x24, PllConfigHi1 = 0x28,
    PllOut = 1u << 0, PllResetN = 1u << 2, PllLock = 1u << 31, PllRun = 1,
    PllGmuL = 0x00440034, PllGmuAlpha = 0x1555, PllGmuOutputs = 0x407,
    RootUpdate = 1u << 0, RootForce = 1u << 1, RootOff = 1u << 31,
    RootConfigMask = 0x10371f, GmuRoot500MHz = 0x201, HubRoot200MHz = 0x505,
    GdscCollapse = 1u << 0, GdscOverride = (1u << 1) | (1u << 2),
    GdscRetain = 1u << 11, GdscOn = 1u << 31,
    GdscWaitMask = 0xfff000, GdscWait = 0x228000,
    ClockEnable = 1u << 0, ClockHwcg = 1u << 1, ClockOff = 1u << 31,
    ClockFsmMask = 7u << 28, ClockFsmOn = 2u << 28,
};

typedef enum { GateOnly, GateDelay, GatePoll, GateHwcg } GateMode;

static const struct {
    Sm8750GpuPowerBlock block;
    uint32_t reg;
    GateMode mode;
} branches[] = {
    {Sm8750GpuGpucc, 0x90bc, GateDelay},  // GPUCC AHB
    {Sm8750GpuGpucc, 0x9010, GatePoll},   // DEMET
    {Sm8750GpuGpucc, 0x90d4, GatePoll},   // CX GMU
    {Sm8750GpuGpucc, 0x90e4, GatePoll},   // CXO
    {Sm8750GpuGpucc, 0x93e8, GatePoll},   // HUB AON
    {Sm8750GpuGpucc, 0x90e8, GatePoll},   // HUB CX INT
    {Sm8750GpuGpucc, 0x7000, GatePoll},   // HLOS GPU SMMU
    {Sm8750GpuGcc, GccDdrGpuAxi, GateOnly},
    {Sm8750GpuGcc, GccGpuMemnoc, GateHwcg},
};

static const uint32_t critical_gates[] = {0x9004, 0x9008, 0x9064, 0x90cc, 0x93a4, 0x93a8};

typedef struct {
    Sm8750GpuPower *state;
    const Sm8750GpuPowerIo *io;
} Power;

static Sm8750GpuPowerStatus
read_reg(Power *p, Sm8750GpuPowerBlock block, uint32_t reg)
{
    p->state->block = block;
    p->state->reg = reg;
    p->state->value = UINT32_MAX;
    if (p->io->read32(p->io->context, block, reg, &p->state->value) ||
        p->state->value == UINT32_MAX) {
        return Sm8750GpuPowerIoError;
    }
    return Sm8750GpuPowerOk;
}

static Sm8750GpuPowerStatus
write_reg(Power *p, Sm8750GpuPowerBlock block, uint32_t reg, uint32_t value)
{
    p->state->block = block;
    p->state->reg = reg;
    p->state->value = value;
    return p->io->write32(p->io->context, block, reg, value) ? Sm8750GpuPowerIoError : Sm8750GpuPowerOk;
}

static Sm8750GpuPowerStatus
wait_us(Power *p, unsigned us)
{
    return p->io->delay_us(p->io->context, us) ? Sm8750GpuPowerIoError : Sm8750GpuPowerOk;
}

static Sm8750GpuPowerStatus
update(Power *p, Sm8750GpuPowerBlock block, uint32_t reg, uint32_t mask, uint32_t bits)
{
    Sm8750GpuPowerStatus error = read_reg(p, block, reg);
    if (error || (p->state->value & mask) == bits) return error;
    return write_reg(p, block, reg, (p->state->value & ~mask) | bits);
}

static Sm8750GpuPowerStatus
poll(Power *p, Sm8750GpuPowerBlock block, uint32_t reg, uint32_t mask, uint32_t bits, unsigned limit)
{
    for (unsigned elapsed = 0; ; ++elapsed) {
        Sm8750GpuPowerStatus error = read_reg(p, block, reg);
        if (error || (p->state->value & mask) == bits) return error;
        if (elapsed == limit) return limit ? Sm8750GpuPowerTimeout : Sm8750GpuPowerUnavailable;
        if ((error = wait_us(p, 1))) return error;
    }
}

static Sm8750GpuPowerStatus
configure_root(Power *p, uint32_t reg, uint32_t cfg)
{
    const Sm8750GpuPowerBlock cc = Sm8750GpuGpucc;
    Sm8750GpuPowerStatus error = read_reg(p, cc, reg);
    if (error) return error;
    if (p->state->value & (RootUpdate | RootForce)) return Sm8750GpuPowerUnavailable;

    if ((error = update(p, cc, reg, RootForce, RootForce)) ||
        (error = poll(p, cc, reg, RootOff | RootForce, RootForce, 500)) ||
        (error = update(p, cc, reg + 4, RootConfigMask, cfg)) ||
        (error = poll(p, cc, reg + 4, RootConfigMask, cfg, 0)) ||
        (error = update(p, cc, reg, RootUpdate, RootUpdate)) ||
        (error = poll(p, cc, reg, RootUpdate, 0, 500)) ||
        (error = update(p, cc, reg, RootForce, 0))) {
        return error;
    }
    return poll(p, cc, reg, RootForce, 0, 500);
}

static Sm8750GpuPowerStatus
configure_pll(Power *p)
{
    const Sm8750GpuPowerBlock cc = Sm8750GpuGpucc;
    static const struct { uint32_t reg, value; } config[] = {
        {PllL, PllGmuL}, {PllAlpha, PllGmuAlpha}, {PllConfig, 0x19660387},
        {PllConfigHi, 0x098060a0}, {PllConfigHi1, 0xb416cb20},
        {PllUser, 0x400}, {PllUserHi, 2},
    };
    Sm8750GpuPowerStatus error = read_reg(p, cc, PllMode);
    if (error) return error;
    if (p->state->value & PllOut) {
        // Adopt only a locked, matching inherited PLL and preserve it on stop.
        if ((p->state->value & (PllLock | PllResetN)) != (PllLock | PllResetN)) {
            return Sm8750GpuPowerUnavailable;
        }
        if ((error = poll(p, cc, PllOpmode, UINT32_MAX, PllRun, 0)) ||
            (error = poll(p, cc, PllL, 0xffff, PllGmuL & 0xffff, 0)) ||
            (error = poll(p, cc, PllAlpha, 0xffff, PllGmuAlpha, 0))) return error;
        return poll(p, cc, PllUser, 0x2003c07, PllGmuOutputs, 0);
    }

    p->state->started_pll = 1;
    if ((error = write_reg(p, cc, PllOpmode, 0))) return error;
    for (size_t i = 0; i < sizeof(config) / sizeof(config[0]); ++i) {
        if ((error = write_reg(p, cc, config[i].reg, config[i].value))) return error;
    }
    if ((error = update(p, cc, PllMode, PllResetN, PllResetN)) ||
        (error = write_reg(p, cc, PllOpmode, PllRun)) ||
        (error = poll(p, cc, PllMode, PllLock, PllLock, 1500)) ||
        (error = update(p, cc, PllUser, 7, 7)) ||
        (error = update(p, cc, PllMode, PllOut, PllOut))) return error;
    return poll(p, cc, PllMode, PllLock | PllOut, PllLock | PllOut, 0);
}

static Sm8750GpuPowerStatus
enable_branch(Power *p, unsigned index)
{
    Sm8750GpuPowerBlock block = branches[index].block;
    uint32_t reg = branches[index].reg;
    GateMode mode = branches[index].mode;
    Sm8750GpuPowerStatus error = read_reg(p, block, reg);
    if (error) return error;
    if (!(p->state->value & ClockEnable)) {
        p->state->acquired_branches |= 1u << index;
        if ((error = write_reg(p, block, reg, p->state->value | ClockEnable))) return error;
    }

    if (mode == GateOnly || mode == GateDelay) {
        if ((error = poll(p, block, reg, ClockEnable, ClockEnable, 0))) return error;
        return mode == GateDelay ? wait_us(p, 10) : Sm8750GpuPowerOk;
    }
    for (unsigned elapsed = 0; ; ++elapsed) {
        if ((error = read_reg(p, block, reg))) return error;
        uint32_t value = p->state->value;
        if ((value & ClockEnable) &&
            (!(value & ClockOff) || (value & ClockFsmMask) == ClockFsmOn ||
             (mode == GateHwcg && (value & ClockHwcg)))) return Sm8750GpuPowerOk;
        if (elapsed == 500) return Sm8750GpuPowerTimeout;
        if ((error = wait_us(p, 1))) return error;
    }
}

static bool
valid_io(const Sm8750GpuPowerIo *io)
{
    return io && io->read32 && io->write32 && io->delay_us && io->check_rails && io->update_gpll_votes;
}

Sm8750GpuPowerStatus
Sm8750GpuPowerOn(Sm8750GpuPower *state, const Sm8750GpuPowerIo *io)
{
    if (!state || !valid_io(io) || state->phase != Sm8750GpuPowerIdle) return Sm8750GpuPowerInvalid;
    *state = (Sm8750GpuPower){.step = Sm8750GpuStepRailCheck};
    if (io->check_rails(io->context)) return Sm8750GpuPowerUnavailable;
    Power p = {state, io};
    const Sm8750GpuPowerBlock cc = Sm8750GpuGpucc, gcc = Sm8750GpuGcc;
    Sm8750GpuPowerStatus error;

    state->phase = Sm8750GpuPowerStarting;
    state->step = Sm8750GpuStepAhb;
    if ((error = update(&p, gcc, GccCfgAhb, ClockEnable, ClockEnable)) ||
        (error = poll(&p, gcc, GccCfgAhb, ClockEnable, ClockEnable, 0)) ||
        (error = wait_us(&p, 10))) return error;

    state->step = Sm8750GpuStepCx;
    if ((error = read_reg(&p, cc, CxHardware))) return error;
    if (state->value & GdscOn) return Sm8750GpuPowerUnavailable;
    if ((error = read_reg(&p, cc, CxGdsc))) return error;
    if ((state->value & (GdscCollapse | GdscOverride)) != GdscCollapse) return Sm8750GpuPowerUnavailable;
    if ((error = update(&p, cc, CxBcr, 1, 0)) ||
        (error = update(&p, cc, GmuBcr, 1, 0)) ||
        (error = wait_us(&p, 1)) ||
        (error = poll(&p, cc, CxBcr, 1, 0, 0)) ||
        (error = poll(&p, cc, GmuBcr, 1, 0, 0)) ||
        (error = update(&p, cc, CxGdsc, GdscWaitMask | GdscCollapse, GdscWait)) ||
        (error = wait_us(&p, 1)) ||
        (error = poll(&p, cc, CxHardware, GdscOn, GdscOn, 2000)) ||
        (error = poll(&p, cc, CxGdsc, GdscCollapse, 0, 0)) ||
        (error = wait_us(&p, 1)) ||
        (error = update(&p, cc, CxGdsc, GdscRetain, GdscRetain)) ||
        (error = poll(&p, cc, CxGdsc, GdscRetain, GdscRetain, 0))) return error;

    state->step = Sm8750GpuStepGpll0;
    state->block = gcc;
    state->reg = GccClockVote;
    state->value = GpllVotes;
    if (io->update_gpll_votes(io->context, GpllVotes, GpllVotes, &state->prior_gpll_votes)) {
        return Sm8750GpuPowerIoError;
    }
    if ((error = poll(&p, gcc, GccClockVote, GpllVotes, GpllVotes, 0)) ||
        (error = wait_us(&p, 10)) ||
        (error = poll(&p, gcc, PllMode, PllLock, PllLock, 500)) ||
        (error = poll(&p, gcc, PllL, 0xffff, 0x1f, 0)) ||
        (error = poll(&p, gcc, PllAlpha, 0xffff, 0x4000, 0))) return error;

    state->step = Sm8750GpuStepPll0;
    if ((error = configure_pll(&p))) return error;

    state->step = Sm8750GpuStepRoots;
    if ((error = configure_root(&p, GmuRoot, GmuRoot500MHz)) ||
        (error = configure_root(&p, HubRoot, HubRoot200MHz))) return error;

    state->step = Sm8750GpuStepBranches;
    for (size_t i = 0; i < sizeof(critical_gates) / sizeof(critical_gates[0]); ++i) {
        if ((error = update(&p, cc, critical_gates[i], ClockEnable, ClockEnable))) return error;
    }
    for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); ++i) {
        if ((error = enable_branch(&p, i))) return error;
    }

    state->step = Sm8750GpuStepDone;
    state->phase = Sm8750GpuPowerRunning;
    return Sm8750GpuPowerOk;
}

Sm8750GpuPowerStatus
Sm8750GpuPowerOff(Sm8750GpuPower *state, const Sm8750GpuPowerIo *io)
{
    if (!state || !valid_io(io) || state->phase != Sm8750GpuPowerRunning) return Sm8750GpuPowerInvalid;
    Power p = {state, io};
    const Sm8750GpuPowerBlock cc = Sm8750GpuGpucc;
    Sm8750GpuPowerStatus error;

    state->phase = Sm8750GpuPowerStopping;
    state->step = Sm8750GpuStepBranches;
    for (unsigned i = sizeof(branches) / sizeof(branches[0]); i-- > 0;) {
        if (!(state->acquired_branches & (1u << i))) continue;
        if ((error = update(&p, branches[i].block, branches[i].reg, ClockEnable, 0)) ||
            (error = poll(&p, branches[i].block, branches[i].reg, ClockEnable, 0, 500))) return error;
    }
    if ((error = wait_us(&p, 10))) return error;

    state->step = Sm8750GpuStepRoots;
    if ((error = configure_root(&p, HubRoot, 0)) ||
        (error = configure_root(&p, GmuRoot, 0))) return error;

    state->step = Sm8750GpuStepCx;
    if ((error = update(&p, cc, CxGdsc, GdscCollapse, GdscCollapse)) ||
        (error = poll(&p, cc, CxGdsc, GdscCollapse, GdscCollapse, 0)) ||
        (error = wait_us(&p, 500)) ||
        (error = poll(&p, cc, CxHardware, GdscOn, 0, 2000))) return error;

    state->step = Sm8750GpuStepPll0;
    if (state->started_pll &&
        ((error = update(&p, cc, PllMode, PllOut, 0)) ||
         (error = poll(&p, cc, PllMode, PllOut, 0, 0)) ||
         (error = update(&p, cc, PllUser, 7, 0)) ||
         (error = write_reg(&p, cc, PllOpmode, 0)))) return error;

    state->step = Sm8750GpuStepGpll0;
    state->block = Sm8750GpuGcc;
    state->reg = GccClockVote;
    state->value = state->prior_gpll_votes & GpllVotes;
    uint32_t ignored;
    if (io->update_gpll_votes(io->context, GpllVotes, state->value, &ignored)) return Sm8750GpuPowerIoError;
    if ((error = poll(&p, Sm8750GpuGcc, GccClockVote, GpllVotes,
                      state->prior_gpll_votes & GpllVotes, 0)) ||
        (error = wait_us(&p, 10))) return error;

    state->step = Sm8750GpuStepDone;
    state->phase = Sm8750GpuPowerIdle;
    return Sm8750GpuPowerOk;
}
