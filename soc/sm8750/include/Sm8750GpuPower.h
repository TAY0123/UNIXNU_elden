/* SPDX-License-Identifier: MIT */
#ifndef SM8750_GPU_POWER_H
#define SM8750_GPU_POWER_H

#include <stdint.h>

typedef enum {
    Sm8750GpuGcc,
    Sm8750GpuGpucc,
} Sm8750GpuPowerBlock;

typedef struct {
    void *context;
    int (*read32)(void *, Sm8750GpuPowerBlock, uint32_t, uint32_t *);
    int (*write32)(void *, Sm8750GpuPowerBlock, uint32_t, uint32_t);
    int (*delay_us)(void *, unsigned);
    int (*check_rails)(void *);
    /* Serialized RMW of GCC_APCS_CLOCK_BRANCH_ENA_VOTE. Write only mask,
     * return its prior bits, and fail if the owning lock is unavailable. */
    int (*update_gpll_votes)(void *, uint32_t mask, uint32_t value, uint32_t *prior);
} Sm8750GpuPowerIo;

typedef enum {
    Sm8750GpuPowerOk,
    Sm8750GpuPowerInvalid,
    Sm8750GpuPowerUnavailable,
    Sm8750GpuPowerIoError,
    Sm8750GpuPowerTimeout,
} Sm8750GpuPowerStatus;

typedef enum {
    Sm8750GpuPowerIdle,
    Sm8750GpuPowerStarting,
    Sm8750GpuPowerRunning,
    Sm8750GpuPowerStopping,
} Sm8750GpuPowerPhase;

typedef enum {
    Sm8750GpuStepRailCheck,
    Sm8750GpuStepAhb,
    Sm8750GpuStepCx,
    Sm8750GpuStepGpll0,
    Sm8750GpuStepPll0,
    Sm8750GpuStepRoots,
    Sm8750GpuStepBranches,
    Sm8750GpuStepDone,
} Sm8750GpuPowerStep;

typedef struct {
    Sm8750GpuPowerPhase phase;
    Sm8750GpuPowerStep step;
    Sm8750GpuPowerBlock block;
    uint32_t reg;
    uint32_t value;
    uint32_t acquired_branches;
    uint32_t prior_gpll_votes;
    uint32_t started_pll;
} Sm8750GpuPower;

/* Caller serializes access, owns the idle CX/GMU clock tree (including the
 * two GPU GPLL0 vote bits) and holds MX/CX votes at least LOW_SVS throughout.
 * check_rails verifies the rail votes. All callbacks return zero on success.
 * MMIO offsets are block-relative and writes must complete before returning.
 * Start requires a zeroed/idle session. After a failed transition retain
 * all resources until the platform recovers the controller or resets.
 * Stop requires quiesced GMU and GX, with DMA stopped and translations
 * drained. Stop parks both roots on XO before disabling a PLL or vote.
 * Always-on gates stay enabled. Existing branch enables and GPLL votes
 * are preserved. The domain and clock rates are owned by this sequence.
 * No GX, firmware or SMMU programming is done here. */
Sm8750GpuPowerStatus Sm8750GpuPowerOn(Sm8750GpuPower *, const Sm8750GpuPowerIo *);
Sm8750GpuPowerStatus Sm8750GpuPowerOff(Sm8750GpuPower *, const Sm8750GpuPowerIo *);

#endif
