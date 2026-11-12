// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef ELDEN_NOVATEK_SPI4_H
#define ELDEN_NOVATEK_SPI4_H

#ifdef __cplusplus
extern "C" {
#endif

#include "GeniSpi.h"

enum { ELDEN_TOUCH_SPI4_BUSY = -16 };

struct elden_touch_spi4 {
    struct geni_io gcc;
    struct geni_io tlmm;
    struct geni_spi spi;
    uint32_t added_ahb_votes;
    uint32_t root_config;
    uint32_t pin_config[5];
    uint32_t irq_config;
    unsigned acquired;
};

// The provider maps GCC, TLMM, QUP1 and SE4 and supplies ordered MMIO.
// Zero-initialize ownership state. The provider must serialize start, stop
// and shared GCC clock votes, and keep wrapper votes owned for the lifetime
// of their clients. If using SE DMA, the provider supplies qualified buffers.
int elden_touch_spi4_start(struct elden_touch_spi4 *bus);

// Arm the falling-edge status latch after the controller is ready. The
// caller must own GPIO 162 and serialize this with report reads and stop.
int elden_touch_spi4_arm_events(struct elden_touch_spi4 *bus);

// Return 1 for an acknowledged event, 0 for idle, or a negative error.
// Acknowledge before reading SPI so an edge during the transfer stays latched.
int elden_touch_spi4_take_event(struct elden_touch_spi4 *bus);

// Quiesce SPI transfers before stopping. A failed cleanup leaves ownership
// recorded so the provider can retry without releasing live resources.
int elden_touch_spi4_stop(struct elden_touch_spi4 *bus);

#ifdef __cplusplus
}
#endif
#endif
