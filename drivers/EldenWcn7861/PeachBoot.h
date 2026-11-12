/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef ELDEN_PEACH_BOOT_H
#define ELDEN_PEACH_BOOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct elden_peach_image {
    const uint8_t *data;
    size_t size;
};

struct elden_peach_firmware {
    struct elden_peach_image peripheral;
    struct elden_peach_image peripheral_nvm;
    struct elden_peach_image tme;
};

struct elden_peach_uart {
    void *context;
    uint64_t (*now_ns)(void *);
    /* Transfer all bytes by the absolute deadline. Send waits for wire drain. */
    int (*send)(void *, const uint8_t *, size_t, uint64_t);
    int (*receive)(void *, uint8_t *, size_t, uint64_t);
    int (*set_baud)(void *, uint32_t);
    int (*set_flow)(void *, bool);
    /* Sleep against the same monotonic clock as now_ns. May wake early. */
    void (*wait_until)(void *, uint64_t);
};

enum elden_peach_stage {
    ELDEN_PEACH_INPUT,
    ELDEN_PEACH_WAKE,
    ELDEN_PEACH_PERIPHERAL_VERSION,
    ELDEN_PEACH_BAUD,
    ELDEN_PEACH_PERIPHERAL_PATCH,
    ELDEN_PEACH_TME,
    ELDEN_PEACH_ACTIVATE,
    ELDEN_PEACH_HCI_VERSION,
    ELDEN_PEACH_DONE,
};

enum elden_peach_error {
    ELDEN_PEACH_BAD_INPUT = -1,
    ELDEN_PEACH_UART_ERROR = -2,
    ELDEN_PEACH_BAD_PACKET = -3,
    ELDEN_PEACH_WRONG_VERSION = -4,
    ELDEN_PEACH_REJECTED = -5,
    ELDEN_PEACH_TIMEOUT = -6,
};

struct elden_peach_result {
    enum elden_peach_stage stage;
    int uart_error;
    uint8_t controller_status;
    uint32_t product;
    uint32_t soc;
    uint16_t rom;
    uint16_t patch;
};

/* Own the powered UART exclusively at 115200 with hardware flow enabled.
 * On success it remains at 8 Mbaud. The caller owns reset after failure. */
int elden_peach_boot(const struct elden_peach_uart *, const struct elden_peach_firmware *,
                    struct elden_peach_result *);

#endif
