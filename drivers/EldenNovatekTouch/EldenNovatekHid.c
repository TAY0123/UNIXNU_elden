// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "EldenNovatekHid.h"
#include "GeniSpi.h"

// USB HID Digitizers page. A finger carries Tip Switch, Untouch, Contact
// Identifier and two absolute axes. Use the tablet's native coordinate range.
#define HID_U16(value) ((value) & 0xff), ((value) >> 8)
#define HID_FINGER \
    0x09, 0x22, 0xa1, 0x02,                         /* Finger, Logical */ \
    0x09, 0x42, 0x09, 0x34, 0x15, 0, 0x25, 1,      /* Tip Switch, Untouch */ \
    0x75, 1, 0x95, 2, 0x81, 0x02,                  /* Two flag bits */ \
    0x75, 6, 0x95, 1, 0x81, 0x03,                  /* Padding */ \
    0x09, 0x51, 0x25, ELDEN_TOUCH_SLOTS,            /* Contact Identifier */ \
    0x75, 8, 0x81, 0x02, \
    0x05, 0x01, 0x09, 0x30,                        /* Generic Desktop X */ \
    0x26, HID_U16(ELDEN_TOUCH_MAX_X), 0x75, 16, 0x81, 0x02, \
    0x09, 0x31, 0x26, HID_U16(ELDEN_TOUCH_MAX_Y), 0x81, 0x02, \
    0x05, 0x0d, 0xc0,

static const uint8_t descriptor[] = {
    0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01,             // Digitizer Touch Screen, Application
    0x85, ELDEN_TOUCH_HID_REPORT_ID,
    HID_FINGER HID_FINGER HID_FINGER HID_FINGER HID_FINGER
    HID_FINGER HID_FINGER HID_FINGER HID_FINGER HID_FINGER
    0x09, 0x54, 0x25, ELDEN_TOUCH_SLOTS,            // Contact Count
    0x75, 8, 0x95, 1, 0x81, 0x02, 0xc0,
};

_Static_assert(ELDEN_TOUCH_SLOTS == 10, "descriptor describes ten contacts");

const uint8_t *
elden_touch_hid_descriptor(size_t *bytes)
{
    if (bytes) {
        *bytes = sizeof(descriptor);
    }
    return descriptor;
}

int
elden_touch_hid_submit(struct elden_touch_hid *state, const struct elden_touch_frame *frame,
    elden_touch_hid_sink sink, void *context)
{
    if (!state || !frame || !sink || (frame->active_slots >> ELDEN_TOUCH_SLOTS) != 0) {
        return GENI_ERR_PARAM;
    }

    unsigned affected = state->delivered.active_slots | frame->active_slots;
    if (!affected) {
        return 0;
    }
    uint8_t report[ELDEN_TOUCH_HID_REPORT_BYTES] = {ELDEN_TOUCH_HID_REPORT_ID};
    unsigned count = 0;
    for (unsigned slot = 0; slot < ELDEN_TOUCH_SLOTS; ++slot) {
        unsigned bit = 1u << slot;
        if (!(affected & bit)) {
            continue;
        }
        int down = (frame->active_slots & bit) != 0;
        const struct elden_touch_contact *contact = down ? &frame->contacts[slot] :
            &state->delivered.contacts[slot];
        if (contact->x > ELDEN_TOUCH_MAX_X || contact->y > ELDEN_TOUCH_MAX_Y) {
            return GENI_ERR_PROTO;
        }
        uint8_t *entry = report + 1 + count++ * ELDEN_TOUCH_HID_CONTACT_BYTES;
        entry[0] = down ? 1 : 2;
        entry[1] = (uint8_t)(slot + 1);
        entry[2] = (uint8_t)contact->x;
        entry[3] = (uint8_t)(contact->x >> 8);
        entry[4] = (uint8_t)contact->y;
        entry[5] = (uint8_t)(contact->y >> 8);
    }
    report[sizeof(report) - 1] = (uint8_t)count;
    int result = sink(context, report, sizeof(report));
    if (result == 0) {
        state->delivered = *frame;
    }
    return result;
}

int
elden_touch_hid_poll(struct elden_touch_hid *state, struct geni_spi *spi,
    elden_touch_hid_sink sink, void *context)
{
    if (!state || !sink) {
        return GENI_ERR_PARAM;
    }
    struct elden_touch_frame frame;
    int result = elden_touch_read_frame(spi, &frame);
    return result ? result : elden_touch_hid_submit(state, &frame, sink, context);
}
