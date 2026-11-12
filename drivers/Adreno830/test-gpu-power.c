/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Sm8750GpuPower.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    GccVote = 0x52000, GccAhb = 0x71004, GccMemnoc = 0x71010,
    GccDdr = 0x71150, Pll = 0, Opmode = 4, PllL = 0x10, PllAlpha = 0x14,
    PllUser = 0x18, CxGdsc = 0x9080, CxHw = 0x9094, GmuRoot = 0x9318,
    HubRoot = 0x93ec, GmuClock = 0x90d4, HubClock = 0x90e8, SmmuClock = 0x7000,
    Off = 1u << 31, BitPll0 = 1u << 15, BitPll0Div = 1u << 16,
    OtherVote = 1u << 4, Pll0Votes = BitPll0 | BitPll0Div,
};

typedef struct {
    uint32_t gcc[0x80000 / 4];
    uint32_t gpucc[0xa000 / 4];
    uint32_t latched[2];
    unsigned now, cx_changed, pll_run, ahb_ready, operations, fail_at, writes, vote_updates;
    bool rails_missing, cx_stuck_off, cx_stuck_on, pll_stuck, vote_busy;
    uint32_t blocked_root, stuck_branch;
} Model;

static Model model;
static Sm8750GpuPower state;

static unsigned root_index(uint32_t reg)
{
    return reg == GmuRoot ? 0 : 1;
}

static bool cx_on(const Model *m)
{
    if (m->cx_stuck_on) return true;
    if (m->cx_stuck_off) return false;
    bool collapse = m->gpucc[CxGdsc / 4] & 1u;
    return (m->now >= m->cx_changed) != collapse;
}

static bool pll_locked(const Model *m)
{
    return !m->pll_stuck && (m->gpucc[Pll / 4] & 4) && m->gpucc[Opmode / 4] == 1 &&
        m->now >= m->pll_run;
}

static bool source_live(const Model *m, uint32_t cfg)
{
    switch ((cfg >> 8) & 7) {
    case 0: return true;
    case 2: return pll_locked(m) && (m->gpucc[Pll / 4] & 1) && (m->gpucc[PllUser / 4] & 2);
    case 5: return m->gcc[GccVote / 4] & BitPll0;
    default: return false;
    }
}

static bool gated(uint32_t reg)
{
    return reg == GmuClock || reg == HubClock || reg == SmmuClock || reg == 0x90e4 ||
        reg == 0x9010 || reg == 0x93e8;
}

static void verify_access(const Model *m, Sm8750GpuPowerBlock block, uint32_t reg)
{
    assert(!(reg & 3));
    if (block == Sm8750GpuGpucc) {
        assert(reg < sizeof(m->gpucc) && (m->gcc[GccAhb / 4] & 1));
        assert(m->now >= m->ahb_ready);
    } else {
        assert(block == Sm8750GpuGcc && reg < sizeof(m->gcc));
    }
}

static int read32(void *context, Sm8750GpuPowerBlock block, uint32_t reg, uint32_t *value)
{
    Model *m = context;
    if (++m->operations == m->fail_at) return -1;
    verify_access(m, block, reg);
    if (block == Sm8750GpuGcc) {
        *value = m->gcc[reg / 4];
        return 0;
    }

    uint32_t *word = &m->gpucc[reg / 4];
    *value = *word;
    if (reg == CxHw) *value = cx_on(m) ? Off : 0;
    if (reg == Pll) *value = (*word & ~Off) | (pll_locked(m) ? Off : 0);
    if (reg == GmuRoot || reg == HubRoot) {
        unsigned index = root_index(reg);
        if ((*word & 3) == 3 && m->blocked_root != reg &&
            source_live(m, m->latched[index]) && source_live(m, word[1])) {
            m->latched[index] = word[1] & 0x10371f;
            *word &= ~1u;
        }
        *value = *word;
        if (!(*word & 2) || !source_live(m, m->latched[index])) *value |= Off;
    }
    if (gated(reg)) {
        bool on = cx_on(m) && (*word & 1) && m->stuck_branch != reg;
        if (reg == GmuClock) on = on && source_live(m, m->latched[0]);
        if (reg == HubClock || reg == 0x93e8) on = on && source_live(m, m->latched[1]);
        *value = (*word & ~Off) | (on ? 0 : Off);
    }
    return 0;
}

static int write32(void *context, Sm8750GpuPowerBlock block, uint32_t reg, uint32_t value)
{
    Model *m = context;
    if (++m->operations == m->fail_at) return -1;
    verify_access(m, block, reg);
    ++m->writes;
    if (block == Sm8750GpuGcc) {
        assert(reg != GccVote);
        if (reg == GccAhb) m->ahb_ready = m->now + 10;
        m->gcc[reg / 4] = value;
        return 0;
    }
    if (reg == CxGdsc && ((m->gpucc[reg / 4] ^ value) & 1u)) {
        if (value & 1) assert(!(m->latched[0] | m->latched[1]));
        m->cx_changed = m->now + 3;
    }
    if (reg == Opmode && value == 1) m->pll_run = m->now + 20;
    if (reg == Pll && (m->gpucc[reg / 4] & 1) && !(value & 1)) {
        assert((m->latched[0] >> 8 & 7) != 2 && !cx_on(m));
    }
    if ((reg == GmuClock || reg == HubClock) && (value & 1)) {
        assert(cx_on(m) && m->latched[reg == GmuClock ? 0 : 1] ==
            (reg == GmuClock ? 0x201u : 0x505u));
    }
    if (reg == GmuRoot || reg == HubRoot || reg == Pll) value &= ~Off;
    m->gpucc[reg / 4] = reg == 0x9008 ? Off : value;
    return 0;
}

static int delay_us(void *context, unsigned us)
{
    Model *m = context;
    if (++m->operations == m->fail_at) return -1;
    m->now += us;
    return 0;
}

static int rails(void *context)
{
    Model *m = context;
    return ++m->operations == m->fail_at || m->rails_missing;
}

static int votes(void *context, uint32_t mask, uint32_t value, uint32_t *prior)
{
    Model *m = context;
    assert(mask == Pll0Votes && !(value & ~mask));
    if (++m->operations == m->fail_at || m->vote_busy) return -1;
    if (!(value & BitPll0)) assert((m->latched[1] >> 8 & 7) != 5 && !cx_on(m));
    uint32_t *word = &m->gcc[GccVote / 4];
    *prior = *word & mask;
    *word = (*word & ~mask) | value;
    ++m->vote_updates;
    return 0;
}

static const Sm8750GpuPowerIo io = {&model, read32, write32, delay_us, rails, votes};

static void setup(void)
{
    memset(&model, 0, sizeof(model));
    state = (Sm8750GpuPower){0};
    model.gcc[Pll / 4] = Off;
    model.gcc[PllL / 4] = 0x1f;
    model.gcc[PllAlpha / 4] = 0x4000;
    model.gcc[GccVote / 4] = OtherVote | BitPll0;
    model.gcc[GccMemnoc / 4] = Off | 2;
    model.gpucc[CxGdsc / 4] = 0x0222f801;
    model.gpucc[0x90bc / 4] = 0x08000000;
    model.gpucc[GmuClock / 4] = 0x88002001;
    model.gpucc[0x90e4 / 4] = 0x88000001;
    model.gpucc[HubClock / 4] = 0x88000001;
}

static void check_running(void)
{
    assert(state.phase == Sm8750GpuPowerRunning && state.step == Sm8750GpuStepDone);
    assert(cx_on(&model) && pll_locked(&model));
    assert(model.latched[0] == 0x201 && model.latched[1] == 0x505);
    assert(model.gpucc[PllL / 4] == 0x00440034 && model.gpucc[PllAlpha / 4] == 0x1555);
    assert((model.gpucc[PllUser / 4] & 0x407) == 0x407);
    assert(model.gpucc[SmmuClock / 4] & 1);
    assert(model.gcc[GccVote / 4] == (OtherVote | Pll0Votes));
}

int main(void)
{
    setup();
    assert(Sm8750GpuPowerOn(&state, &io) == Sm8750GpuPowerOk);
    check_running();
    unsigned start_ops = model.operations;
    assert(Sm8750GpuPowerOn(&state, &io) == Sm8750GpuPowerInvalid);
    assert(Sm8750GpuPowerOff(&state, &io) == Sm8750GpuPowerOk);
    unsigned stop_ops = model.operations - start_ops;
    assert(state.phase == Sm8750GpuPowerIdle && !cx_on(&model));
    assert(!model.latched[0] && !model.latched[1]);
    assert(!pll_locked(&model) && !(model.gpucc[Pll / 4] & 1));
    assert(model.gpucc[GmuClock / 4] & 1);
    assert(model.gpucc[HubClock / 4] & 1);
    assert(!(model.gpucc[SmmuClock / 4] & 1));
    assert(model.gcc[GccVote / 4] == (OtherVote | BitPll0));
    assert(model.gcc[GccAhb / 4] & 1);
    assert(Sm8750GpuPowerOn(&state, &io) == Sm8750GpuPowerOk);
    check_running();

    setup();
    model.rails_missing = true;
    assert(Sm8750GpuPowerOn(&state, &io) == Sm8750GpuPowerUnavailable);
    assert(!model.writes && state.phase == Sm8750GpuPowerIdle);

    setup();
    model.cx_stuck_off = true;
    assert(Sm8750GpuPowerOn(&state, &io) == Sm8750GpuPowerTimeout);
    assert(state.reg == CxHw && !model.vote_updates);

    setup();
    model.cx_stuck_on = true;
    assert(Sm8750GpuPowerOn(&state, &io) == Sm8750GpuPowerUnavailable);
    assert(state.reg == CxHw && model.writes == 1);

    setup();
    model.gcc[PllL / 4] = 0x29;
    assert(Sm8750GpuPowerOn(&state, &io) == Sm8750GpuPowerUnavailable);
    assert(state.step == Sm8750GpuStepGpll0 && state.reg == PllL);

    setup();
    model.pll_stuck = true;
    assert(Sm8750GpuPowerOn(&state, &io) == Sm8750GpuPowerTimeout);
    assert(state.step == Sm8750GpuStepPll0 && !model.latched[0]);

    setup();
    model.vote_busy = true;
    assert(Sm8750GpuPowerOn(&state, &io) == Sm8750GpuPowerIoError);
    assert(state.reg == GccVote && model.gcc[GccVote / 4] == (OtherVote | BitPll0));

    setup();
    model.stuck_branch = SmmuClock;
    assert(Sm8750GpuPowerOn(&state, &io) == Sm8750GpuPowerTimeout);
    assert(state.reg == SmmuClock);

    setup();
    assert(Sm8750GpuPowerOn(&state, &io) == Sm8750GpuPowerOk);
    model.blocked_root = HubRoot;
    assert(Sm8750GpuPowerOff(&state, &io) == Sm8750GpuPowerTimeout);
    assert(state.reg == HubRoot && pll_locked(&model) && model.vote_updates == 1);

    setup();
    assert(Sm8750GpuPowerOn(&state, &io) == Sm8750GpuPowerOk);
    model.cx_stuck_on = true;
    assert(Sm8750GpuPowerOff(&state, &io) == Sm8750GpuPowerTimeout);
    assert(state.reg == CxHw && pll_locked(&model) && model.vote_updates == 1);

    setup();
    model.gpucc[Pll / 4] = 5;
    model.gpucc[Opmode / 4] = 1;
    model.gpucc[PllL / 4] = 0x00440034;
    model.gpucc[PllAlpha / 4] = 0x1555;
    model.gpucc[PllUser / 4] = 0x407;
    assert(Sm8750GpuPowerOn(&state, &io) == Sm8750GpuPowerOk);
    assert(!state.started_pll);
    assert(Sm8750GpuPowerOff(&state, &io) == Sm8750GpuPowerOk && pll_locked(&model));

    for (unsigned i = 1; i <= start_ops; ++i) {
        setup();
        model.fail_at = i;
        assert(Sm8750GpuPowerOn(&state, &io) != Sm8750GpuPowerOk);
        assert(model.operations == i);
    }
    for (unsigned i = 1; i <= stop_ops; ++i) {
        setup();
        assert(Sm8750GpuPowerOn(&state, &io) == Sm8750GpuPowerOk);
        model.fail_at = model.operations + i;
        assert(Sm8750GpuPowerOff(&state, &io) != Sm8750GpuPowerOk);
        assert(state.phase == Sm8750GpuPowerStopping && model.operations == model.fail_at);
    }
    printf("SM8750 GPU power model passed with %u startup and %u shutdown injection points\n",
        start_ops, stop_ops);
}
