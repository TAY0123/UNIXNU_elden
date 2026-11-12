// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "EldenNovatekSpi4.h"
#include "Sm8750Qup1Resources.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

static const unsigned pin_gpios[] = {48, 49, 50, 51, 162};

struct registers {
    uint32_t votes, root, config, irq_config;
    uint32_t pins[5], se[0x1000 / 4];
    bool hold_update, hold_branch, refuse_pin;
    unsigned writes, delays;
};

static uint32_t
gcc_read(void *context, uint32_t reg)
{
    struct registers *r = context;
    switch (reg) {
    case SM8750_GCC_QUP1_VOTES:
        return r->votes;
    case SM8750_GCC_QUP1_SE4_ROOT:
        if (!r->hold_update) {
            r->root &= ~1u;
        }
        return r->root | ((r->root & 2) ? 0 : 1u << 31);
    case SM8750_GCC_QUP1_SE4_CONFIG:
        return r->config;
    case SM8750_GCC_QUP1_SE4_BRANCH:
        return (r->votes & SM8750_GCC_QUP1_SE4_VOTE) && !r->hold_branch ? 0 : 1u << 31;
    case SM8750_GCC_QUP1_M_AHB:
        return (r->votes & (1u << 20)) ? 0 : 1u << 31;
    case SM8750_GCC_QUP1_S_AHB:
        return (r->votes & (1u << 21)) ? (1u << 31) | 2u : 1u << 31;
    default:
        assert(false);
        return 0;
    }
}

static void
gcc_write(void *context, uint32_t reg, uint32_t value)
{
    struct registers *r = context;
    ++r->writes;
    switch (reg) {
    case SM8750_GCC_QUP1_VOTES:
        r->votes = value;
        break;
    case SM8750_GCC_QUP1_SE4_ROOT:
        r->root = value & 3u;
        break;
    case SM8750_GCC_QUP1_SE4_CONFIG:
        assert((r->root & 2) && !(r->votes & SM8750_GCC_QUP1_SE4_VOTE));
        r->config = value;
        break;
    default:
        assert(false);
    }
}

static void
delay(void *context, unsigned us)
{
    struct registers *r = context;
    assert(us && r->delays < 5000);
    ++r->delays;
}

static unsigned
pin_index(uint32_t reg)
{
    for (unsigned i = 0; i < sizeof(pin_gpios) / sizeof(pin_gpios[0]); ++i) {
        if (reg == pin_gpios[i] * SM8750_TLMM_GPIO_STRIDE) {
            return i;
        }
    }
    assert(false);
    return 0;
}

static uint32_t
tlmm_read(void *context, uint32_t reg)
{
    struct registers *r = context;
    if (reg == 162 * SM8750_TLMM_GPIO_STRIDE + 8) {
        return r->irq_config;
    }
    return r->pins[pin_index(reg)];
}

static void
tlmm_write(void *context, uint32_t reg, uint32_t value)
{
    struct registers *r = context;
    unsigned i = pin_index(reg);
    ++r->writes;
    if (!r->refuse_pin || i != 2) {
        r->pins[i] = value;
    }
}

static uint32_t
se_read(void *context, uint32_t reg)
{
    struct registers *r = context;
    assert((r->votes & SM8750_GCC_QUP1_SE4_VOTE) && !r->hold_branch);
    assert(reg % 4 == 0 && reg < sizeof(r->se));
    return r->se[reg / 4];
}

static void
se_write(void *context, uint32_t reg, uint32_t value)
{
    struct registers *r = context;
    (void)se_read(context, reg);
    ++r->writes;
    r->se[reg / 4] = value;
}

static uint32_t
wrapper_read(void *context, uint32_t reg)
{
    struct registers *r = context;
    assert(reg == QUPV3_HW_VER_REG);
    assert((r->votes & SM8750_GCC_QUP1_AHB_VOTES) == SM8750_GCC_QUP1_AHB_VOTES);
    return 0x300a0000;
}

static struct elden_touch_spi4
setup(struct registers *r)
{
    *r = (struct registers){
        .votes = 1u << 20 | 1u << 27,
        .config = 1u << 20,
        .pins = {0x62000300, 0x62000002, 0x62000020, 0x62000302, 0x62000303},
    };
    r->se[GENI_FW_REVISION_RO / 4] = GENI_PROTO_SPI << GENI_FW_PROTO_SHIFT;
    r->se[SE_HW_PARAM_0 / 4] = 32u << 24 | 16u << 16;
    return (struct elden_touch_spi4){
        .gcc = {r, gcc_read, gcc_write, delay},
        .tlmm = {r, tlmm_read, tlmm_write, NULL},
        .spi = {
            .se = {r, se_read, se_write, delay},
            .wrapper = {r, wrapper_read, NULL, NULL},
        },
    };
}

int
main(void)
{
    struct registers r;
    struct elden_touch_spi4 bus = setup(&r);
    struct registers before = r;
    assert(elden_touch_spi4_start(&bus) == 0);
    assert(r.votes == (before.votes | SM8750_GCC_QUP1_AHB_VOTES | SM8750_GCC_QUP1_SE4_VOTE));
    assert(r.config == (before.config | 1u) && r.root == 0);
    assert(r.pins[0] == 0x62000285 && r.pins[1] == 0x62000085);
    assert(r.pins[2] == 0x62000085 && r.pins[3] == 0x62000287 && r.pins[4] == 0x620000c3);
    assert(r.se[GENI_SER_M_CLK_CFG / 4] == (2u << GENI_CLK_DIV_SHIFT | GENI_SER_CLK_EN));
    assert(r.se[SE_IRQ_EN / 4] == 0 && !r.irq_config);
    unsigned writes = r.writes;
    assert(elden_touch_spi4_start(&bus) == ELDEN_TOUCH_SPI4_BUSY && r.writes == writes);
    r.se[SE_GENI_STATUS / 4] = GENI_STATUS_M_ACTIVE;
    assert(elden_touch_spi4_stop(&bus) == ELDEN_TOUCH_SPI4_BUSY && r.writes == writes);
    r.se[SE_GENI_STATUS / 4] = 0;
    r.votes |= 1u << 28;
    assert(elden_touch_spi4_stop(&bus) == 0 && !bus.acquired);
    assert(r.votes == (before.votes | 1u << 28) && r.config == before.config);
    assert(memcmp(r.pins, before.pins, sizeof(r.pins)) == 0);
    assert(elden_touch_spi4_start(&bus) == 0 && elden_touch_spi4_stop(&bus) == 0);
    assert(r.votes == (before.votes | 1u << 28));

    bus = setup(&r);
    r.votes |= SM8750_GCC_QUP1_SE4_VOTE;
    assert(elden_touch_spi4_start(&bus) == ELDEN_TOUCH_SPI4_BUSY && r.writes == 0);
    bus = setup(&r);
    r.irq_config = 1;
    assert(elden_touch_spi4_start(&bus) == ELDEN_TOUCH_SPI4_BUSY && r.writes == 0);
    bus = setup(&r);
    r.config |= 1u << 8;
    assert(elden_touch_spi4_start(&bus) == GENI_ERR_NOTSUP && r.writes == 0);

    bus = setup(&r);
    r.hold_branch = true;
    assert(elden_touch_spi4_start(&bus) == GENI_ERR_TIMEOUT && !bus.acquired);
    assert(r.votes == before.votes && r.config == before.config);
    bus = setup(&r);
    r.se[GENI_FW_REVISION_RO / 4] = 0;
    assert(elden_touch_spi4_start(&bus) == GENI_ERR_PROTO && !bus.acquired);
    assert(r.votes == before.votes && r.config == before.config);
    bus = setup(&r);
    r.se[GENI_IF_DISABLE_RO / 4] = GENI_FIFO_IF_DISABLE;
    assert(elden_touch_spi4_start(&bus) == GENI_ERR_NOTSUP && !bus.acquired);
    assert(r.votes == before.votes && r.config == before.config);
    bus = setup(&r);
    r.refuse_pin = true;
    assert(elden_touch_spi4_start(&bus) == GENI_ERR_IO && !bus.acquired);
    assert(r.votes == before.votes && memcmp(r.pins, before.pins, sizeof(r.pins)) == 0);

    bus = setup(&r);
    r.hold_update = true;
    assert(elden_touch_spi4_start(&bus) == GENI_ERR_TIMEOUT && bus.acquired);
    assert((r.votes & SM8750_GCC_QUP1_AHB_VOTES) == SM8750_GCC_QUP1_AHB_VOTES);
    assert(elden_touch_spi4_start(&bus) == ELDEN_TOUCH_SPI4_BUSY);
    r.hold_update = false;
    assert(elden_touch_spi4_stop(&bus) == 0 && !bus.acquired);
    assert(r.votes == before.votes && r.config == before.config);
}
