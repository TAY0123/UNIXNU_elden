/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "PeachBoot.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct peer {
    const struct elden_peach_firmware *firmware;
    uint64_t now;
    uint64_t baud_request;
    uint8_t rx[128];
    size_t rx_start, rx_end;
    size_t offsets[3];
    unsigned segments[3];
    unsigned sends, wakes, wake_acks, rate_changes;
    uint8_t patch_status, tme_status;
    bool flow, patch_ready, tme_ready, activated;
    bool suppress_patch_ready, noisy, bad_version, fail_baud;
};

static void
queue(struct peer *p, const uint8_t *bytes, size_t count)
{
    assert(count <= sizeof(p->rx) - p->rx_end);
    memcpy(p->rx + p->rx_end, bytes, count);
    p->rx_end += count;
}

static void
complete(struct peer *p, uint8_t opcode, uint8_t operation, uint8_t ssid, uint8_t status)
{
    uint8_t response[] = {0x34, 0, 0xff, 8, 0xf0, 1, 1, opcode, 0xff, status, operation, ssid};
    if (opcode == 0xf1) {
        response[3] = 7;
        queue(p, response, sizeof(response) - 1);
    } else {
        queue(p, response, sizeof(response));
    }
}

static void
notification(struct peer *p, uint8_t event, uint8_t status, uint8_t ssid)
{
    const uint8_t response[] = {0x34, 0, 0xff, 4, 0xf0, event, status, ssid};
    queue(p, response, sizeof(response));
}

static void
segment(struct peer *p, const uint8_t *tx, size_t count)
{
    assert(count >= 9 && tx[7] == count - 8);
    unsigned file;
    if (tx[6] == 3) {
        assert(p->wakes == 2 && p->tme_status == 0);
        file = 2;
    } else {
        assert(tx[6] == 0 && p->wakes == 1 && p->patch_status == 0);
        file = p->offsets[0] == p->firmware->peripheral.size ? 1u : 0u;
    }
    const struct elden_peach_image files[] = {
        p->firmware->peripheral, p->firmware->peripheral_nvm, p->firmware->tme,
    };
    size_t start = p->offsets[file];
    size_t length = files[file].size - start;
    if (length > 243) {
        length = 243;
    }
    assert(length != 0 && length == tx[7]);
    for (size_t i = 0; i < length; ++i) {
        size_t offset = start + i;
        uint8_t value = files[file].data[offset];
        if (file == 1 && offset == 93) {
            value = 0x15;
        }
        if (file == 1 && offset == 111) {
            value |= 1;
        }
        assert(tx[8 + i] == value);
    }
    p->offsets[file] += length;
    ++p->segments[file];
    bool last = p->offsets[file] == files[file].size;
    if (file == 2 && last) {
        notification(p, 3, 0, 3);
        p->tme_ready = true;
    }
    if (file == 1 || last) {
        complete(p, 0xf0, 6, tx[6], 0);
    }
}

static int
send_bytes(void *context, const uint8_t *tx, size_t count, uint64_t deadline)
{
    struct peer *p = context;
    assert(count && p->now < deadline);
    ++p->sends;
    if (count == 1) {
        if (tx[0] == 0xfc) {
            ++p->wake_acks;
            return 0;
        }
        assert(tx[0] == 0xfd && p->wakes < 2);
        ++p->wakes;
        if (!p->noisy) {
            const uint8_t wake[] = {0xfd, 0xfc};
            queue(p, wake, sizeof(wake));
        }
        return 0;
    }
    if (tx[0] == 1) {
        const uint8_t command[] = {1, 0, 0xfc, 1, 0x19};
        const uint8_t version[] = {4, 0x0e, 18, 1, 0, 0xfc, 0, 0x19, 0, 0x1e, 0,
            0, 0, 0x14, 0, 0, 2, 0, 2, 0x21, 0x40};
        assert(p->activated && count == sizeof(command) && !memcmp(tx, command, count));
        queue(p, version, sizeof(version));
        return 0;
    }
    assert(count >= 6 && tx[0] == 0x31 && tx[1] == 0 && tx[3] == 0xff && tx[4] == count - 5);
    if (tx[2] == 0xf0) {
        assert(count >= 7);
        if (tx[5] == 5) {
            uint8_t version[25] = {0x34, 0, 0xff, 21, 0xf0, 1, 1, 0xf0, 0xff, 0, 5,
                tx[6], 0x21, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0x21, 0x40, 0};
            assert(count == 7 && ((tx[6] == 0 && p->wakes == 1) || (tx[6] == 3 && p->wakes == 2)));
            if (p->bad_version) {
                version[12] = 0x20;
            }
            queue(p, version, sizeof(version));
        } else if (tx[5] == 8) {
            assert(count == 8 && tx[7] == 0 && p->rate_changes == 1);
            assert((tx[6] == 0 && p->wakes == 1) || (tx[6] == 3 && p->wakes == 2));
            complete(p, 0xf0, 8, tx[6], tx[6] == 0 ? p->patch_status : p->tme_status);
        } else {
            assert(tx[5] == 6);
            segment(p, tx, count);
        }
        return 0;
    }
    assert(tx[2] == 0xf1);
    if (tx[5] == 2) {
        assert(count == 7 && tx[6] == 0x15 && p->wakes == 1 && !p->flow);
        p->baud_request = p->now;
        complete(p, 0xf1, 2, 0, 1);
    } else if (tx[5] == 3) {
        assert(count == 6 && p->offsets[1] == p->firmware->peripheral_nvm.size);
        complete(p, 0xf1, 3, 0, 0);
        if (!p->suppress_patch_ready) {
            notification(p, 3, 0, 0);
            p->patch_ready = true;
        }
    } else {
        assert(count == 8 && tx[5] == 0 && tx[6] == 1 && tx[7] == 1);
        assert((p->patch_ready || p->patch_status == 0x17) && (p->tme_ready || p->tme_status));
        uint8_t status[] = {0x34, 0, 0xff, 6, 0xf0, 0, 0, 1, 0xf1, 0xff};
        if (p->patch_status == 0x17) {
            status[6] = 0x17;
        } else {
            notification(p, 2, 1, 1);
        }
        queue(p, status, sizeof(status));
        p->activated = true;
    }
    return 0;
}

static int
receive_bytes(void *context, uint8_t *rx, size_t count, uint64_t deadline)
{
    struct peer *p = context;
    assert(p->now < deadline);
    if (p->noisy) {
        assert(count == 1);
        *rx = 0xfe;
        p->now += 1000000;
        return 0;
    }
    if (count > p->rx_end - p->rx_start) {
        p->now = deadline;
        return -ETIMEDOUT;
    }
    memcpy(rx, p->rx + p->rx_start, count);
    p->rx_start += count;
    if (p->rx_start == p->rx_end) {
        p->rx_start = p->rx_end = 0;
    }
    return 0;
}

static uint64_t
time_ns(void *context)
{
    return ((struct peer *)context)->now;
}

static int
baud(void *context, uint32_t rate)
{
    struct peer *p = context;
    assert(rate == 8000000 && !p->flow && p->now - p->baud_request == 20000000);
    ++p->rate_changes;
    return p->fail_baud ? -EIO : 0;
}

static int
flow(void *context, bool enabled)
{
    struct peer *p = context;
    assert(p->flow != enabled);
    p->flow = enabled;
    return 0;
}

static void
wait_until(void *context, uint64_t end)
{
    struct peer *p = context;
    assert(p->now < end);
    p->now = end;
}

static int
boot(struct peer *p, const struct elden_peach_firmware *firmware, struct elden_peach_result *result)
{
    const struct elden_peach_uart uart = {p, time_ns, send_bytes, receive_bytes, baud, flow, wait_until};
    p->firmware = firmware;
    p->flow = true;
    return elden_peach_boot(&uart, firmware, result);
}

static struct elden_peach_image
load(const char *path, size_t length)
{
    FILE *file = fopen(path, "rb");
    uint8_t *data = malloc(length);
    assert(file && data && fread(data, 1, length, file) == length && fgetc(file) == EOF && !ferror(file));
    fclose(file);
    return (struct elden_peach_image){data, length};
}

int
main(int argc, char **argv)
{
    assert(argc == 4);
    const struct elden_peach_firmware firmware = {
        load(argv[1], 19884), load(argv[2], 741), load(argv[3], 56752),
    };
    uint8_t baud_before = firmware.peripheral_nvm.data[93];
    uint8_t sleep_before = firmware.peripheral_nvm.data[111];
    struct elden_peach_result result;
    struct peer p = {0};
    assert(boot(&p, &firmware, &result) == 0 && result.stage == ELDEN_PEACH_DONE);
    assert(result.product == 0x1e && result.patch == 0x14 && result.rom == 0x0200 && result.soc == 0x40210200);
    assert(p.segments[0] == 82 && p.segments[1] == 4 && p.segments[2] == 234);
    assert(p.wakes == 2 && p.wake_acks == 2 && p.flow && p.rx_start == p.rx_end);
    assert(firmware.peripheral_nvm.data[93] == baud_before && firmware.peripheral_nvm.data[111] == sleep_before);

    p = (struct peer){.patch_status = 0x17, .tme_status = 0x17};
    assert(boot(&p, &firmware, &result) == 0 && result.stage == ELDEN_PEACH_DONE);
    assert(!p.segments[0] && !p.segments[1] && !p.segments[2]);
    p = (struct peer){.patch_status = 0x17, .tme_status = 5};
    assert(boot(&p, &firmware, &result) == 0 && !p.segments[2]);

    p = (struct peer){.bad_version = true};
    assert(boot(&p, &firmware, &result) == ELDEN_PEACH_WRONG_VERSION);
    assert(result.stage == ELDEN_PEACH_PERIPHERAL_VERSION && !p.rate_changes);
    p = (struct peer){.fail_baud = true};
    assert(boot(&p, &firmware, &result) == ELDEN_PEACH_UART_ERROR);
    assert(result.stage == ELDEN_PEACH_BAUD && result.uart_error == -EIO && p.flow);
    p = (struct peer){.suppress_patch_ready = true};
    assert(boot(&p, &firmware, &result) == ELDEN_PEACH_UART_ERROR);
    assert(result.stage == ELDEN_PEACH_PERIPHERAL_PATCH && result.uart_error == -ETIMEDOUT && p.wakes == 1);
    p = (struct peer){.noisy = true};
    assert(boot(&p, &firmware, &result) == ELDEN_PEACH_TIMEOUT);
    assert(result.stage == ELDEN_PEACH_WAKE && p.now == 100000000 && p.sends == 1);

    struct elden_peach_firmware truncated = firmware;
    --truncated.peripheral_nvm.size;
    p = (struct peer){0};
    assert(boot(&p, &truncated, &result) == ELDEN_PEACH_BAD_INPUT);
    assert(!p.sends && result.stage == ELDEN_PEACH_INPUT);

    free((void *)firmware.peripheral.data);
    free((void *)firmware.peripheral_nvm.data);
    free((void *)firmware.tme.data);
    puts("PASS: Peach peripheral and TME download, HCI version, warm paths and failure bounds");
    return 0;
}
