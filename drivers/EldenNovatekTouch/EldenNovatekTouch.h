// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef ELDEN_NOVATEK_TOUCH_H
#define ELDEN_NOVATEK_TOUCH_H

#include <stdint.h>

enum {
    ELDEN_TOUCH_SLOTS = 10,
    ELDEN_TOUCH_MAX_X = 19040,
    ELDEN_TOUCH_MAX_Y = 30400,
};

struct geni_spi;

struct elden_touch_contact {
    uint16_t x;
    uint16_t y;
    uint8_t width;
};

struct elden_touch_frame {
    uint16_t active_slots;
    struct elden_touch_contact contacts[ELDEN_TOUCH_SLOTS];
};

// Call with the SPI bus serialized and the controller's firmware running.
// A valid frame replaces every slot. On error the previous frame stays intact.
int elden_touch_read_frame(struct geni_spi *spi, struct elden_touch_frame *frame);

#endif
