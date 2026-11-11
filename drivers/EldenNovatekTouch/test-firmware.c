// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "EldenNovatekTouch.h"
#include "GeniSpi.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct test_controller {
    const uint8_t *image;
    uint32_t page;
    unsigned transfers;
    unsigned operation;
    size_t written;
    unsigned auto_copy_reads;
    unsigned reset_reads;
    unsigned delays;
    unsigned fail_at;
    int wrong_chip;
    int stuck_copy;
    int wrong_pid;
};

enum { FIRST_FIRMWARE_WRITE = 12, LAST_FIRMWARE_WRITE = 15 };

static void
expect_write(const uint8_t *data, size_t bytes, uint32_t value, unsigned count)
{
    assert(bytes == count);
    for (unsigned i = 0; i < count; i++) {
        assert(data[i] == (uint8_t)(value >> (i * 8)));
    }
}

int
geni_spi_transfer(struct geni_spi *spi, const uint8_t *tx, uint8_t *rx, size_t len, int keep_cs)
{
    struct test_controller *controller = spi->se.ctx;
    assert(keep_cs == 0 && tx != NULL && len > 0);
    if (++controller->transfers == controller->fail_at) {
        return GENI_ERR_TIMEOUT;
    }
    if (rx == NULL && len == 3 && tx[0] == 0xff) {
        controller->page = ((uint32_t)tx[1] << 15) | ((uint32_t)tx[2] << 7);
        return 0;
    }

    uint32_t address = controller->page | (tx[0] & 0x7f);
    unsigned operation = controller->operation;
    if (operation >= FIRST_FIRMWARE_WRITE && operation <= LAST_FIRMWARE_WRITE) {
        const uint32_t addresses[] = {0, 0x100000, 0x1158c0, 0x1159c0};
        const size_t offsets[] = {0x250, 0x22000, 0, 0x100};
        const size_t sizes[] = {0x1fc2c, 0x158c0, 0x100, 0x100};
        unsigned part = operation - FIRST_FIRMWARE_WRITE;
        size_t remaining = sizes[part] - controller->written;
        size_t expected = remaining > 15360 ? 15360 : remaining;
        assert(rx == NULL && tx[0] >= 0x80 && len == expected + 1);
        assert(address == addresses[part] + controller->written);
        assert(memcmp(tx + 1, controller->image + offsets[part] + controller->written, expected) == 0);
        controller->written += expected;
        if (controller->written == sizes[part]) {
            controller->written = 0;
            controller->operation++;
        }
        return 0;
    }

    const uint32_t addresses[] = {
        0x7fff80, 0x1fb43e, 0x1fb104, 0x1fb104, 0x1fb43e, 0x1fcc30,
        0x1fcc34, 0x1fcc38, 0x1fcc40, 0x1fcc44, 0x1fcc48, 0x1fc825,
        [16] = 0x1fc825, 0x130960, 0x130950, 0x1fb50d, 0x130960, 0x130978,
    };
    assert(operation < sizeof(addresses) / sizeof(addresses[0]));
    assert(address == addresses[operation]);
    controller->operation++;
    if (rx == NULL) {
        const unsigned widths[] = {1, 1, 6, 0, 1, 3, 3, 4, 3, 3, 4, 1, [17] = 6, 2, 1};
        const uint32_t values[] = {
            0x5a, 0x69, 0, 0, 0x69, 0, 0x1fc2b, 0xc9bbe411, 0x100000,
            0x158bf, 0x47059e2f, 0x56, [17] = 0, 0xae, 1,
        };
        assert(tx[0] >= 0x80 && operation < sizeof(widths) / sizeof(widths[0]));
        if (widths[operation] == 6) {
            assert(len == 7 && memcmp(tx + 1, "\0\0\0\0\0\0", 6) == 0);
        } else {
            assert(widths[operation] != 0);
            expect_write(tx + 1, len - 1, values[operation], widths[operation]);
        }
        return 0;
    }

    assert(tx[0] < 0x80);
    for (size_t i = 1; i < len; i++) {
        assert(tx[i] == 0);
    }
    memset(rx, 0, len);
    rx[0] = 0xa5;
    rx[1] = 0x5a;
    switch (operation) {
    case 3:
        assert(len == 8);
        rx[2] = controller->wrong_chip ? 0 : 0x15;
        rx[5] = 0x23;
        rx[6] = 0x65;
        rx[7] = 0x03;
        break;
    case 16:
        assert(len == 3);
        if (++controller->auto_copy_reads == 1 || controller->stuck_copy) {
            rx[2] = 0x56;
            controller->operation--;
        }
        break;
    case 20:
        assert(len == 3);
        if (++controller->reset_reads == 1) {
            controller->operation--;
        } else {
            rx[2] = 0xa1;
        }
        break;
    case 21:
        assert(len == 40);
        rx[2] = 0x1a;
        rx[3] = 0xe5;
        rx[14] = 0xf1;
        rx[15] = 1;
        rx[36] = controller->wrong_pid ? 0 : 0x97;
        rx[37] = 0x60;
        break;
    default:
        assert(0);
    }
    return 0;
}

static void
delay_us(void *context, unsigned delay)
{
    struct test_controller *controller = context;
    assert(delay == 1000 || delay == 5000 || delay == 10000 || delay == 11000);
    controller->delays++;
}

int
main(int argc, char **argv)
{
    static uint8_t image[ELDEN_TOUCH_FW_BYTES];
    static uint8_t scratch[ELDEN_TOUCH_FW_SCRATCH_BYTES];
    assert(argc == 2);
    FILE *input = fopen(argv[1], "rb");
    assert(input != NULL && fread(image, 1, sizeof(image), input) == sizeof(image));
    assert(fgetc(input) == EOF && !ferror(input));
    assert(fclose(input) == 0);

    struct test_controller controller = {.image = image};
    struct geni_spi spi = {.se = {.ctx = &controller, .delay_us = delay_us}};
    struct elden_touch_firmware_info info = {0};
    assert(elden_touch_upload_firmware(&spi, image, sizeof(image) - 1, scratch, sizeof(scratch), &info) == GENI_ERR_PROTO);
    assert(elden_touch_upload_firmware(&spi, image, sizeof(image), scratch, sizeof(scratch) - 1, &info) == GENI_ERR_PARAM);
    image[0xf0] ^= 1;
    assert(elden_touch_upload_firmware(&spi, image, sizeof(image), scratch, sizeof(scratch), &info) == GENI_ERR_PROTO);
    image[0xf0] ^= 1;
    assert(controller.transfers == 0);

    controller.wrong_chip = 1;
    assert(elden_touch_upload_firmware(&spi, image, sizeof(image), scratch, sizeof(scratch), &info) == GENI_ERR_NODEV);
    assert(controller.operation == 4 && info.product == 0);
    controller = (struct test_controller){.image = image, .fail_at = 32};
    assert(elden_touch_upload_firmware(&spi, image, sizeof(image), scratch, sizeof(scratch), &info) == GENI_ERR_TIMEOUT);
    assert(controller.transfers == 32 && controller.operation < 16 && info.product == 0);
    controller = (struct test_controller){.image = image, .stuck_copy = 1};
    assert(elden_touch_upload_firmware(&spi, image, sizeof(image), scratch, sizeof(scratch), &info) == GENI_ERR_TIMEOUT);
    assert(controller.auto_copy_reads == 200 && controller.operation == 16 && info.product == 0);
    controller = (struct test_controller){.image = image, .wrong_pid = 1};
    assert(elden_touch_upload_firmware(&spi, image, sizeof(image), scratch, sizeof(scratch), &info) == GENI_ERR_NODEV);
    assert(controller.operation == 22 && info.product == 0);

    controller = (struct test_controller){.image = image};
    assert(elden_touch_upload_firmware(&spi, image, sizeof(image), scratch, sizeof(scratch), &info) == 0);
    assert(controller.operation == 22 && controller.auto_copy_reads == 2 && controller.reset_reads == 2);
    assert(controller.delays == 7 && info.product == 0x6097 && info.version == 0x1a);
    assert(info.revision == 1 && info.protocol == 0xf1);
}
