// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "EldenNovatekTouch.h"
#include "EldenNovatekHid.h"
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

struct test_sink {
    uint8_t report[ELDEN_TOUCH_HID_REPORT_BYTES];
    unsigned calls;
    int result;
};

static int
capture_report(void *context, const uint8_t *report, size_t bytes)
{
    struct test_sink *sink = context;
    assert(bytes == sizeof(sink->report));
    assert(report[0] == ELDEN_TOUCH_HID_REPORT_ID);
    memcpy(sink->report, report, bytes);
    ++sink->calls;
    return sink->result;
}

static void
check_contact(const struct test_sink *sink, unsigned record, unsigned flags, unsigned id,
    unsigned x, unsigned y)
{
    const uint8_t *entry = sink->report + 1 + record * ELDEN_TOUCH_HID_CONTACT_BYTES;
    assert(entry[0] == flags && entry[1] == id);
    assert(((unsigned)entry[3] << 8 | entry[2]) == x);
    assert(((unsigned)entry[5] << 8 | entry[4]) == y);
}

static void
test_hid_delivery(void)
{
    struct elden_touch_hid hid = {0};
    struct test_sink sink = {0};
    struct test_device device = {0};
    struct geni_spi spi = {.se.ctx = &device};

    set_contact(device.report, 0, 10, 1, ELDEN_TOUCH_MAX_X, ELDEN_TOUCH_MAX_Y, 1);
    set_contact(device.report, 1, 1, 1, 200, 400, 1);
    set_checksum(device.report);
    assert(elden_touch_hid_poll(&hid, &spi, capture_report, &sink) == 0);
    assert(sink.calls == 1 && sink.report[sizeof(sink.report) - 1] == 2);
    check_contact(&sink, 0, 1, 1, 200, 400);
    check_contact(&sink, 1, 1, 10, ELDEN_TOUCH_MAX_X, ELDEN_TOUCH_MAX_Y);

    device = (struct test_device){0};
    set_contact(device.report, 0, 2, 1, 120, 400, 1);
    set_contact(device.report, 1, 10, 2, ELDEN_TOUCH_MAX_X, ELDEN_TOUCH_MAX_Y, 1);
    set_checksum(device.report);
    sink.result = GENI_ERR_IO;
    assert(elden_touch_hid_poll(&hid, &spi, capture_report, &sink) == GENI_ERR_IO);
    assert(hid.delivered.active_slots == (1u | 1u << 9));
    uint8_t retry[sizeof(sink.report)];
    memcpy(retry, sink.report, sizeof(retry));
    sink.result = 0;
    device.transfers = 0;
    assert(elden_touch_hid_poll(&hid, &spi, capture_report, &sink) == 0);
    assert(sink.report[sizeof(sink.report) - 1] == 3 && memcmp(sink.report, retry, sizeof(retry)) == 0);
    check_contact(&sink, 0, 2, 1, 200, 400);
    check_contact(&sink, 1, 1, 2, 120, 400);
    check_contact(&sink, 2, 1, 10, ELDEN_TOUCH_MAX_X, ELDEN_TOUCH_MAX_Y);

    device.transfers = 0;
    device.report[64] ^= 1;
    assert(elden_touch_hid_poll(&hid, &spi, capture_report, &sink) == GENI_ERR_PROTO);
    assert(sink.calls == 3 && hid.delivered.active_slots == (1u << 1 | 1u << 9));

    device = (struct test_device){0};
    for (unsigned i = 0; i < ELDEN_TOUCH_SLOTS; ++i) {
        set_contact(device.report, i, i + 1, 1, 100 + i, 200 + i, 1);
    }
    set_checksum(device.report);
    assert(elden_touch_hid_poll(&hid, &spi, capture_report, &sink) == 0);
    assert(sink.report[sizeof(sink.report) - 1] == ELDEN_TOUCH_SLOTS);
    device = (struct test_device){0};
    assert(elden_touch_hid_poll(&hid, &spi, capture_report, &sink) == 0);
    assert(sink.report[sizeof(sink.report) - 1] == ELDEN_TOUCH_SLOTS && hid.delivered.active_slots == 0);
    for (unsigned i = 0; i < ELDEN_TOUCH_SLOTS; ++i) {
        check_contact(&sink, i, 2, i + 1, 100 + i, 200 + i);
    }
    device.transfers = 0;
    assert(elden_touch_hid_poll(&hid, &spi, capture_report, &sink) == 0 && sink.calls == 5);
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
    test_hid_delivery();
}
