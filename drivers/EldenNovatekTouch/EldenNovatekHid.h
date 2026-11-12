// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef ELDEN_NOVATEK_HID_H
#define ELDEN_NOVATEK_HID_H

#ifdef __cplusplus
extern "C" {
#endif

#include "EldenNovatekTouch.h"

enum {
    ELDEN_TOUCH_HID_REPORT_ID = 1,
    ELDEN_TOUCH_HID_CONTACT_BYTES = 6,
    ELDEN_TOUCH_HID_REPORT_BYTES = 2 + ELDEN_TOUCH_SLOTS * ELDEN_TOUCH_HID_CONTACT_BYTES,
};

struct elden_touch_hid {
    struct elden_touch_frame delivered;
};

typedef int (*elden_touch_hid_sink)(void *context, const uint8_t *report, size_t bytes);

const uint8_t *elden_touch_hid_descriptor(size_t *bytes);

// Zero-initialize state and serialize calls with the SPI worker. The sink
// consumes the report synchronously and returns zero on success. A failed
// transfer or delivery leaves the previous delivered frame available for retry.
int elden_touch_hid_submit(struct elden_touch_hid *state, const struct elden_touch_frame *frame,
    elden_touch_hid_sink sink, void *context);
int elden_touch_hid_poll(struct elden_touch_hid *state, struct geni_spi *spi,
    elden_touch_hid_sink sink, void *context);

#ifdef __cplusplus
}
#endif
#endif
