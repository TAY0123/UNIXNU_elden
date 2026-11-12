// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "EldenNovatekSpi4.h"
#include "Sm8750Qup1Resources.h"

enum {
    AcquiredAhb = 1u << 0,
    AcquiredRoot = 1u << 1,
    AcquiredEngine = 1u << 2,
    AcquiredPins = 1u << 3,
    AcquiredEvents = 1u << 4,
    ClockOff = 1u << 31,
    AhbHardwareGating = 1u << 1,
    RootUpdate = 1u << 0,
    RootForceEnable = 1u << 1,
    RootSourceAndMode = 0x3700,
    RootDivider = 0x1f,
    RootTcxoDivideOne = 1,
    TcxoHz = 19200000,
    TouchSpiHz = 9600000,
    PinMuxPullDrive = 0x1ff,
    PinOutputEnable = 1u << 9,
    PinIrqConfig = 0x8,
    PinIrqStatus = 0xc,
    PinIrqEnable = 1u << 0,
    PinIrqPolarity = 1u << 1,
    PinIrqDetection = 3u << 2,
    PinIrqFalling = 2u << 2,
    PinIrqRawStatusEnable = 1u << 4,
    PinIrqWakeEnable = 1u << 7,
    PinIrqExternalEnable = PinIrqEnable | PinIrqWakeEnable,
    PinIrqEventMask = PinIrqPolarity | PinIrqDetection | PinIrqRawStatusEnable,
    PinIrqEventConfig = PinIrqPolarity | PinIrqFalling | PinIrqRawStatusEnable,
    PinIrqPending = 1u << 0,
    TouchIrqGpio = 162,
    GeniSecondaryActive = 1u << 12,
};

// Elden's touch-spi-data-state, touch-spi-cs-state and touch-irq-state.
static const struct {
    unsigned gpio;
    uint32_t mask;
    uint32_t active;
} pins[] = {
    {48, PinMuxPullDrive, 0x85},
    {49, PinMuxPullDrive, 0x85},
    {50, PinMuxPullDrive, 0x85},
    {51, PinMuxPullDrive, 0x87},
    {TouchIrqGpio, PinMuxPullDrive | PinOutputEnable, 0xc3},
};

_Static_assert(sizeof(pins) / sizeof(pins[0]) ==
    sizeof(((struct elden_touch_spi4 *)0)->pin_config) / sizeof(uint32_t),
    "snapshot has one slot per Elden pin");

static uint32_t
read32(const struct geni_io *io, uint32_t reg)
{
    return io->rd(io->ctx, reg);
}

static void
update32(const struct geni_io *io, uint32_t reg, uint32_t mask, uint32_t value)
{
    uint32_t previous = read32(io, reg);
    uint32_t next = (previous & ~mask) | (value & mask);
    if (next != previous) {
        io->wr(io->ctx, reg, next);
    }
}

static int
wait32(const struct geni_io *io, uint32_t reg, uint32_t mask, uint32_t expected)
{
    for (unsigned i = 0; i < 500; ++i) {
        if ((read32(io, reg) & mask) == expected) {
            return 0;
        }
        io->delay_us(io->ctx, 2);
    }
    return GENI_ERR_TIMEOUT;
}

static int
wait_ahb(const struct geni_io *gcc, uint32_t reg)
{
    // Linux clk-branch skips the halt check for hardware-gated AHB clocks.
    return (read32(gcc, reg) & AhbHardwareGating) ? 0 : wait32(gcc, reg, ClockOff, 0);
}

static int
program_root(const struct geni_io *gcc, uint32_t config)
{
    const uint32_t root = SM8750_GCC_QUP1_SE4_ROOT;
    int error = wait32(gcc, root, RootUpdate, 0);
    if (error) {
        return error;
    }
    update32(gcc, root, RootForceEnable, RootForceEnable);
    error = wait32(gcc, root, ClockOff, 0);
    if (!error) {
        update32(gcc, SM8750_GCC_QUP1_SE4_CONFIG, RootSourceAndMode | RootDivider, config);
        error = wait32(gcc, SM8750_GCC_QUP1_SE4_CONFIG, RootSourceAndMode | RootDivider,
            config & (RootSourceAndMode | RootDivider));
    }
    if (!error) {
        update32(gcc, root, RootUpdate, RootUpdate);
        error = wait32(gcc, root, RootUpdate, 0);
    }
    update32(gcc, root, RootForceEnable, 0);
    int released = wait32(gcc, root, RootForceEnable, 0);
    return error ? error : released;
}

static uint32_t
pin_reg(unsigned index)
{
    return pins[index].gpio * SM8750_TLMM_GPIO_STRIDE;
}

static int
set_pins(struct elden_touch_spi4 *bus, int restore)
{
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        uint32_t value = restore ? bus->pin_config[i] : pins[i].active;
        update32(&bus->tlmm, pin_reg(i), pins[i].mask, value);
        if ((read32(&bus->tlmm, pin_reg(i)) & pins[i].mask) != (value & pins[i].mask)) {
            return GENI_ERR_IO;
        }
    }
    return 0;
}

static uint32_t
touch_irq_reg(uint32_t offset)
{
    return TouchIrqGpio * SM8750_TLMM_GPIO_STRIDE + offset;
}

static int
restore_events(struct elden_touch_spi4 *bus)
{
    const struct geni_io *tlmm = &bus->tlmm;
    uint32_t config = touch_irq_reg(PinIrqConfig);
    if (read32(tlmm, config) & PinIrqExternalEnable) {
        return ELDEN_TOUCH_SPI4_BUSY;
    }
    update32(tlmm, config, PinIrqEventMask, bus->irq_config);
    if ((read32(tlmm, config) & PinIrqEventMask) != bus->irq_config) {
        return GENI_ERR_IO;
    }
    tlmm->wr(tlmm->ctx, touch_irq_reg(PinIrqStatus), 0);
    bus->acquired &= ~AcquiredEvents;
    return 0;
}

int
elden_touch_spi4_arm_events(struct elden_touch_spi4 *bus)
{
    if (!bus || !(bus->acquired & AcquiredPins)) {
        return GENI_ERR_PARAM;
    }
    const struct geni_io *tlmm = &bus->tlmm;
    uint32_t config = touch_irq_reg(PinIrqConfig);
    uint32_t current = read32(tlmm, config);
    if ((bus->acquired & AcquiredEvents) || (current & PinIrqExternalEnable)) {
        return ELDEN_TOUCH_SPI4_BUSY;
    }
    bus->irq_config = current & PinIrqEventMask;
    bus->acquired |= AcquiredEvents;
    // RAW_STATUS_EN latches the edge with the GIC and PDC routes disabled.
    update32(tlmm, config, PinIrqEventMask, PinIrqEventConfig);
    if ((read32(tlmm, config) & PinIrqEventMask) != PinIrqEventConfig) {
        int cleanup = restore_events(bus);
        return cleanup ? cleanup : GENI_ERR_IO;
    }
    // Configuring the raw latch can set INTR_STATUS without a real edge.
    tlmm->wr(tlmm->ctx, touch_irq_reg(PinIrqStatus), 0);
    return 0;
}

int
elden_touch_spi4_take_event(struct elden_touch_spi4 *bus)
{
    if (!bus || !(bus->acquired & AcquiredEvents)) {
        return GENI_ERR_PARAM;
    }
    const struct geni_io *tlmm = &bus->tlmm;
    uint32_t config = read32(tlmm, touch_irq_reg(PinIrqConfig));
    if ((config & (PinIrqEventMask | PinIrqExternalEnable)) != PinIrqEventConfig) {
        return ELDEN_TOUCH_SPI4_BUSY;
    }
    uint32_t status = touch_irq_reg(PinIrqStatus);
    if (!(read32(tlmm, status) & PinIrqPending)) {
        return 0;
    }
    tlmm->wr(tlmm->ctx, status, 0);
    return 1;
}

int
elden_touch_spi4_stop(struct elden_touch_spi4 *bus)
{
    if (!bus) {
        return GENI_ERR_PARAM;
    }
    const struct geni_io *gcc = &bus->gcc;
    if (bus->acquired & AcquiredEngine) {
        uint32_t votes = read32(gcc, SM8750_GCC_QUP1_VOTES);
        uint32_t branch = read32(gcc, SM8750_GCC_QUP1_SE4_BRANCH);
        if ((votes & SM8750_GCC_QUP1_SE4_VOTE) && !(branch & ClockOff) &&
            (read32(&bus->spi.se, SE_GENI_STATUS) & (GENI_STATUS_M_ACTIVE | GeniSecondaryActive))) {
            return ELDEN_TOUCH_SPI4_BUSY;
        }
        update32(gcc, SM8750_GCC_QUP1_VOTES, SM8750_GCC_QUP1_SE4_VOTE, 0);
        int error = wait32(gcc, SM8750_GCC_QUP1_VOTES, SM8750_GCC_QUP1_SE4_VOTE, 0);
        if (!error) {
            error = wait32(gcc, SM8750_GCC_QUP1_SE4_BRANCH, ClockOff, ClockOff);
        }
        if (error) {
            return error;
        }
        bus->acquired &= ~AcquiredEngine;
    }
    if (bus->acquired & AcquiredEvents) {
        int error = restore_events(bus);
        if (error) {
            return error;
        }
    }
    if (bus->acquired & AcquiredPins) {
        int error = set_pins(bus, 1);
        if (error) {
            return error;
        }
        bus->acquired &= ~AcquiredPins;
    }
    if (bus->acquired & AcquiredRoot) {
        int error = program_root(gcc, bus->root_config);
        if (error) {
            return error;
        }
        bus->acquired &= ~AcquiredRoot;
    }
    if (bus->acquired & AcquiredAhb) {
        update32(gcc, SM8750_GCC_QUP1_VOTES, bus->added_ahb_votes, 0);
        int error = wait32(gcc, SM8750_GCC_QUP1_VOTES, bus->added_ahb_votes, 0);
        if (error) {
            return error;
        }
        bus->acquired &= ~AcquiredAhb;
    }
    return 0;
}

int
elden_touch_spi4_start(struct elden_touch_spi4 *bus)
{
    if (!bus || !bus->gcc.rd || !bus->gcc.wr || !bus->gcc.delay_us ||
        !bus->tlmm.rd || !bus->tlmm.wr || !bus->spi.se.rd || !bus->spi.se.wr ||
        !bus->spi.se.delay_us || !bus->spi.wrapper.rd) {
        return GENI_ERR_PARAM;
    }
    const struct geni_io *gcc = &bus->gcc;
    uint32_t votes = read32(gcc, SM8750_GCC_QUP1_VOTES);
    if (bus->acquired || (votes & SM8750_GCC_QUP1_SE4_VOTE) ||
        !(read32(gcc, SM8750_GCC_QUP1_SE4_BRANCH) & ClockOff) ||
        (read32(gcc, SM8750_GCC_QUP1_SE4_ROOT) & (RootForceEnable | RootUpdate)) ||
        (read32(&bus->tlmm, touch_irq_reg(PinIrqConfig)) & PinIrqExternalEnable)) {
        return ELDEN_TOUCH_SPI4_BUSY;
    }
    bus->root_config = read32(gcc, SM8750_GCC_QUP1_SE4_CONFIG);
    if (bus->root_config & RootSourceAndMode) {
        // Restore only a qualified parked root. A PLL parent may be off.
        return GENI_ERR_NOTSUP;
    }

    bus->added_ahb_votes = SM8750_GCC_QUP1_AHB_VOTES & ~votes;
    bus->acquired = AcquiredAhb;
    update32(gcc, SM8750_GCC_QUP1_VOTES, SM8750_GCC_QUP1_AHB_VOTES, SM8750_GCC_QUP1_AHB_VOTES);
    int error = wait32(gcc, SM8750_GCC_QUP1_VOTES, SM8750_GCC_QUP1_AHB_VOTES, SM8750_GCC_QUP1_AHB_VOTES);
    if (!error) {
        error = wait_ahb(gcc, SM8750_GCC_QUP1_M_AHB);
    }
    if (!error) {
        error = wait_ahb(gcc, SM8750_GCC_QUP1_S_AHB);
    }
    if (!error) {
        bus->acquired |= AcquiredRoot;
        error = program_root(gcc, RootTcxoDivideOne);
    }
    if (!error) {
        bus->acquired |= AcquiredEngine;
        update32(gcc, SM8750_GCC_QUP1_VOTES, SM8750_GCC_QUP1_SE4_VOTE, SM8750_GCC_QUP1_SE4_VOTE);
        error = wait32(gcc, SM8750_GCC_QUP1_VOTES, SM8750_GCC_QUP1_SE4_VOTE, SM8750_GCC_QUP1_SE4_VOTE);
    }
    if (!error) {
        error = wait32(gcc, SM8750_GCC_QUP1_SE4_BRANCH, ClockOff, 0);
    }
    if (!error && (read32(&bus->spi.se, SE_GENI_STATUS) & (GENI_STATUS_M_ACTIVE | GeniSecondaryActive))) {
        error = ELDEN_TOUCH_SPI4_BUSY;
    }
    if (!error) {
        // Initialize the controller in SPI mode 0 before connecting the pads.
        bus->spi.poll_only = 1;
        error = geni_spi_init(&bus->spi);
    }
    if (!error) {
        error = geni_spi_set_clock(&bus->spi, TouchSpiHz, TcxoHz, 0);
    }
    if (!error) {
        unsigned divider = TcxoHz / (TouchSpiHz * bus->spi.oversampling);
        uint32_t expected = (divider << GENI_CLK_DIV_SHIFT) | GENI_SER_CLK_EN;
        if ((read32(&bus->spi.se, SE_GENI_CLK_SEL) & 7u) != 0 ||
            read32(&bus->spi.se, GENI_SER_M_CLK_CFG) != expected ||
            read32(&bus->spi.se, SE_IRQ_EN) != 0) {
            error = GENI_ERR_IO;
        }
    }
    if (!error) {
        for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
            bus->pin_config[i] = read32(&bus->tlmm, pin_reg(i));
        }
        bus->acquired |= AcquiredPins;
        error = set_pins(bus, 0);
    }
    if (!error) {
        return 0;
    }
    int cleanup = elden_touch_spi4_stop(bus);
    return cleanup ? cleanup : error;
}
