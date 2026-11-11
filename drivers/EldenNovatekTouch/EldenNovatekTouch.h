// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef ELDEN_NOVATEK_TOUCH_H
#define ELDEN_NOVATEK_TOUCH_H

#include <stddef.h>
#include <stdint.h>

enum {
    ELDEN_TOUCH_SLOTS = 10,
    ELDEN_TOUCH_MAX_X = 19040,
    ELDEN_TOUCH_MAX_Y = 30400,
    ELDEN_TOUCH_FW_BYTES = 253952,
    ELDEN_TOUCH_FW_SCRATCH_BYTES = 15361,
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

struct elden_touch_firmware_info {
    uint16_t product;
    uint8_t version;
    uint8_t revision;
    uint8_t protocol;
};

// The caller owns SPI, panel power, and a separate scratch buffer. Use the
// verified Elden firmware input and call from a context that permits delays.
int elden_touch_upload_firmware(struct geni_spi *spi, const uint8_t *image,
    size_t image_bytes, uint8_t *scratch, size_t scratch_bytes,
    struct elden_touch_firmware_info *info);

// Call with the SPI bus serialized and the controller's firmware running.
// A valid frame replaces every slot. On error the previous frame stays intact.
int elden_touch_read_frame(struct geni_spi *spi, struct elden_touch_frame *frame);

#endif
