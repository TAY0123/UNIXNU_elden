// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "EldenNovatekTouch.h"
#include "GeniSpi.h"

#include <assert.h>
#include <string.h>

struct test_device {
    uint8_t report[108];
    unsigned transfers;
    unsigned fail_at;
};

int
geni_spi_transfer(struct geni_spi *spi, const uint8_t *tx, uint8_t *rx, size_t len, int keep_cs)
{
    struct test_device *device = spi->se.ctx;
    assert(keep_cs == 0);
    unsigned transfer = ++device->transfers;
    if (device->fail_at == transfer) {
        return GENI_ERR_TIMEOUT;
    }
    if (transfer == 1) {
        const uint8_t page[] = {0xff, 0x26, 0x12};
        assert(rx == NULL && len == sizeof(page));
        assert(memcmp(tx, page, sizeof(page)) == 0);
        return 0;
    }

    assert(transfer == 2 && rx != NULL && len == 110);
    for (size_t byte = 0; byte < len; byte++) {
        assert(tx[byte] == 0);
    }
    rx[0] = 0xa5;
    rx[1] = 0x5a;
    memcpy(rx + 2, device->report, sizeof(device->report));
    return 0;
}

static void
set_contact(uint8_t *report, unsigned index, unsigned id, unsigned state,
    unsigned x, unsigned y, uint8_t width)
{
    uint8_t *record = report + index * 6;
    record[0] = (uint8_t)((id << 3) | state);
    record[1] = (uint8_t)(x >> 8);
    record[2] = (uint8_t)x;
    record[3] = (uint8_t)(y >> 8);
    record[4] = (uint8_t)y;
    record[5] = width;
}

static void
set_checksum(uint8_t *report)
{
    unsigned sum = 0;
    for (unsigned byte = 0; byte < 64; byte++) {
        sum += report[byte];
    }
    report[64] = (uint8_t)(0u - sum);
}

int
main(void)
{
    struct test_device device = {0};
    struct geni_spi spi = {.se.ctx = &device};
    struct elden_touch_frame frame = {0};

    assert(elden_touch_read_frame(NULL, &frame) == GENI_ERR_PARAM);
    assert(elden_touch_read_frame(&spi, NULL) == GENI_ERR_PARAM);
    assert(device.transfers == 0);

    set_contact(device.report, 0, 10, 2, ELDEN_TOUCH_MAX_X, ELDEN_TOUCH_MAX_Y, 13);
    set_contact(device.report, 1, 1, 1, 0x1234, 0x5678, 0);
    set_contact(device.report, 2, 11, 1, 50, 100, 1);
    set_contact(device.report, 3, 2, 1, ELDEN_TOUCH_MAX_X + 1, 100, 1);
    set_contact(device.report, 4, 3, 2, 50, ELDEN_TOUCH_MAX_Y + 1, 1);
    set_contact(device.report, 5, 4, 3, 50, 100, 1);
    set_contact(device.report, 6, 0, 1, 50, 100, 1);
    device.report[63] = 0xff;
    device.report[107] = 0x5a;
    set_checksum(device.report);
    assert(elden_touch_read_frame(&spi, &frame) == 0);
    assert(frame.active_slots == ((1u << 9) | 1u));
    assert(frame.contacts[9].x == ELDEN_TOUCH_MAX_X);
    assert(frame.contacts[9].y == ELDEN_TOUCH_MAX_Y && frame.contacts[9].width == 13);
    assert(frame.contacts[0].x == 0x1234 && frame.contacts[0].y == 0x5678);
    assert(frame.contacts[0].width == 1);

    device.transfers = 0;
    device.report[64] ^= 1;
    assert(elden_touch_read_frame(&spi, &frame) == GENI_ERR_PROTO);
    assert(frame.active_slots == ((1u << 9) | 1u) && frame.contacts[9].width == 13);

    for (unsigned fail_at = 1; fail_at <= 2; fail_at++) {
        device.transfers = 0;
        device.fail_at = fail_at;
        assert(elden_touch_read_frame(&spi, &frame) == GENI_ERR_TIMEOUT);
        assert(device.transfers == fail_at && frame.active_slots == ((1u << 9) | 1u));
    }

    device = (struct test_device){0};
    assert(elden_touch_read_frame(&spi, &frame) == 0);
    assert(frame.active_slots == 0 && frame.contacts[9].width == 0);
}
