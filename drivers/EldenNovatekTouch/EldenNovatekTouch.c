// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "EldenNovatekTouch.h"
#include "GeniSpi.h"

enum {
    EVENT_ADDRESS = 0x130900,
    REPORT_BYTES = 108,
    CHECKSUM_BYTES = 65,
    RECORD_BYTES = 6,
    READ_PREFIX_BYTES = 2,
    STATE_ENTER = 1,
    STATE_MOVE = 2,
};

static int
decode_report(const uint8_t *report, struct elden_touch_frame *frame)
{
    unsigned sum = 0;
    for (unsigned byte = 0; byte < CHECKSUM_BYTES; byte++) {
        sum += report[byte];
    }
    if ((uint8_t)sum != 0) {
        return GENI_ERR_PROTO;
    }

    struct elden_touch_frame next = {0};
    for (unsigned index = 0; index < ELDEN_TOUCH_SLOTS; index++) {
        const uint8_t *record = report + index * RECORD_BYTES;
        unsigned id = record[0] >> 3;
        unsigned state = record[0] & 7;
        if (id == 0 || id > ELDEN_TOUCH_SLOTS || (state != STATE_ENTER && state != STATE_MOVE)) {
            continue;
        }

        unsigned x = ((unsigned)record[1] << 8) | record[2];
        unsigned y = ((unsigned)record[3] << 8) | record[4];
        if (x > ELDEN_TOUCH_MAX_X || y > ELDEN_TOUCH_MAX_Y) {
            continue;
        }

        unsigned slot = id - 1;
        next.contacts[slot] = (struct elden_touch_contact){
            .x = (uint16_t)x,
            .y = (uint16_t)y,
            .width = record[5] ? record[5] : 1,
        };
        next.active_slots |= (uint16_t)(1u << slot);
    }
    *frame = next;
    return 0;
}

int
elden_touch_read_frame(struct geni_spi *spi, struct elden_touch_frame *frame)
{
    if (spi == NULL || frame == NULL) {
        return GENI_ERR_PARAM;
    }

    const uint8_t page[] = {0xff, (EVENT_ADDRESS >> 15) & 0xff, (EVENT_ADDRESS >> 7) & 0xff};
    int error = geni_spi_transfer(spi, page, NULL, sizeof(page), 0);
    if (error != 0) {
        return error;
    }

    const uint8_t request[REPORT_BYTES + READ_PREFIX_BYTES] = {EVENT_ADDRESS & 0x7f};
    uint8_t response[sizeof(request)];
    error = geni_spi_transfer(spi, request, response, sizeof(response), 0);
    if (error != 0) {
        return error;
    }
    return decode_report(response + READ_PREFIX_BYTES, frame);
}
