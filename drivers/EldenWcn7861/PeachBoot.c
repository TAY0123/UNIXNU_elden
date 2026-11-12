/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "PeachBoot.h"

/* WCN7861 peripheral and HCI packet layouts from the Elden Linux driver. */
enum {
    PeripheralCommand = 0x31, PeripheralEvent = 0x34, HciEvent = 0x04,
    WakeRequest = 0xfd, WakeAck = 0xfc, SleepIndication = 0xfe,
    EdlOpcode = 0xfff0, GeneralOpcode = 0xfff1, PeachPatched = 0x17,
    EdlVersion = 5, EdlSegment = 6, EdlArbitrate = 8,
    PatchSsid = 0, TmeSsid = 3, Chunk = 243,
    BaudCode = 0x15, OperatingBaud = 8000000,
    ReplyMs = 3000, WakeMs = 100, SwitchMs = 20,
    Millisecond = 1000000,
};

enum notification { NoNotification, PatchReady, TmeReady, Activated };

struct boot {
    const struct elden_peach_uart *uart;
    struct elden_peach_result *result;
    enum notification expected;
    bool notified;
};

struct packet {
    uint8_t type;
    uint8_t data[258];
    size_t size;
};

struct nvm_offsets { size_t baud, sleep; };

static uint16_t
le16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] | (unsigned)p[1] << 8);
}

static uint32_t
le32(const uint8_t *p)
{
    return le16(p) | (uint32_t)le16(p + 2) << 16;
}

static bool
tlv(const struct elden_peach_image *image, uint8_t type)
{
    return image->data && image->size >= 24 && image->size <= 65536 &&
        image->data[0] == type && (le32(image->data) >> 8) == image->size - 4;
}

static bool
validate_firmware(const struct elden_peach_firmware *fw, struct nvm_offsets *offsets)
{
    const struct elden_peach_image *patch = &fw->peripheral;
    const struct elden_peach_image *nvm = &fw->peripheral_nvm;
    const struct elden_peach_image *tme = &fw->tme;
    if (!tlv(patch, 1) || patch->data[14] != 3 ||
        le16(patch->data + 16) != 0x21 || le16(patch->data + 18) != 0x0200 ||
        !tlv(nvm, 2) || !tme->data || tme->size < 52 || tme->size > 65536 ||
        le32(tme->data) != UINT32_C(0x464c457f) || tme->data[4] != 1 ||
        tme->data[5] != 1 || tme->data[6] != 1 || le16(tme->data + 18) != 243) {
        return false;
    }
    for (size_t pos = 4; pos < nvm->size;) {
        if (nvm->size - pos < 12) {
            return false;
        }
        uint16_t tag = le16(nvm->data + pos);
        size_t length = le16(nvm->data + pos + 2);
        if (length > nvm->size - pos - 12) {
            return false;
        }
        if (tag == 17 || tag == 27) {
            size_t *offset = tag == 17 ? &offsets->baud : &offsets->sleep;
            if (*offset || length < 2) {
                return false;
            }
            *offset = pos + 13;
        }
        pos += 12 + length;
    }
    return offsets->baud && offsets->sleep;
}

static uint64_t
deadline(const struct boot *b, unsigned ms)
{
    uint64_t now = b->uart->now_ns(b->uart->context);
    uint64_t interval = (uint64_t)ms * Millisecond;
    return now > UINT64_MAX - interval ? UINT64_MAX : now + interval;
}

static int
uart_result(struct boot *b, int error)
{
    if (!error) {
        return 0;
    }
    b->result->uart_error = error;
    return ELDEN_PEACH_UART_ERROR;
}

static void
note_notification(struct boot *b, const uint8_t *data)
{
    if (b->expected == Activated) {
        b->notified |= data[4] == 2 && data[5] == 1 && data[6] == 1;
    } else if (b->expected != NoNotification) {
        uint8_t ssid = b->expected == PatchReady ? PatchSsid : TmeSsid;
        b->notified |= data[4] == 3 && data[5] == 0 && data[6] == ssid;
    }
}

static int
read_packet(struct boot *b, struct packet *packet, uint64_t end)
{
    const struct elden_peach_uart *uart = b->uart;
    if (uart->now_ns(uart->context) >= end) {
        return ELDEN_PEACH_TIMEOUT;
    }
    int rc = uart->receive(uart->context, &packet->type, 1, end);
    if (rc) {
        return uart_result(b, rc);
    }
    packet->size = 0;
    if (packet->type == WakeRequest) {
        const uint8_t ack = WakeAck;
        return uart_result(b, uart->send(uart->context, &ack, 1, end));
    }
    if (packet->type == WakeAck || packet->type == SleepIndication) {
        return 0;
    }
    if (packet->type != PeripheralEvent && packet->type != HciEvent) {
        return ELDEN_PEACH_BAD_PACKET;
    }
    size_t header = packet->type == PeripheralEvent ? 3u : 2u;
    rc = uart->receive(uart->context, packet->data, header, end);
    if (rc) {
        return uart_result(b, rc);
    }
    size_t length = packet->data[header - 1];
    packet->size = header + length;
    if (length) {
        rc = uart->receive(uart->context, packet->data + header, length, end);
        if (rc) {
            return uart_result(b, rc);
        }
    }
    if (packet->type == PeripheralEvent) {
        const uint8_t *data = packet->data;
        if (packet->size < 7 || data[0] != 0 || data[1] != 0xff || data[3] != 0xf0) {
            return ELDEN_PEACH_BAD_PACKET;
        }
        note_notification(b, data);
    }
    return 0;
}

static int
wake(struct boot *b)
{
    const uint8_t request = WakeRequest;
    uint64_t end = deadline(b, WakeMs);
    int rc = uart_result(b, b->uart->send(b->uart->context, &request, 1, end));
    struct packet packet;
    while (!rc) {
        rc = read_packet(b, &packet, end);
        if (!rc && packet.type == WakeAck) {
            return 0;
        }
    }
    return rc;
}

static int
read_reply(struct boot *b, uint16_t opcode, uint8_t operation, uint8_t ssid)
{
    uint64_t end = deadline(b, ReplyMs);
    for (;;) {
        struct packet packet;
        int rc = read_packet(b, &packet, end);
        if (rc) {
            return rc;
        }
        if (packet.type != PeripheralEvent) {
            continue;
        }
        const uint8_t *data = packet.data;
        if (opcode == GeneralOpcode && operation == 0 && packet.size >= 9 &&
            data[4] == 0 && le16(data + 7) == opcode) {
            b->result->controller_status = data[5];
            return data[5];
        }
        if (packet.size < 10 || data[4] != 1 || le16(data + 6) != opcode ||
            data[9] != operation || (opcode == EdlOpcode && (packet.size < 11 || data[10] != ssid))) {
            continue;
        }
        b->result->controller_status = data[8];
        if (opcode == EdlOpcode && operation == EdlVersion && !data[8] &&
            (packet.size < 24 || le32(data + 11) != 0x21 ||
            le16(data + 17) != 0x0200 || le32(data + 19) != UINT32_C(0x40210200))) {
            return ELDEN_PEACH_WRONG_VERSION;
        }
        return data[8];
    }
}

static int
send_command(struct boot *b, uint8_t *packet, uint16_t opcode, size_t payload)
{
    packet[0] = PeripheralCommand;
    packet[1] = 0;
    packet[2] = (uint8_t)opcode;
    packet[3] = (uint8_t)(opcode >> 8);
    packet[4] = (uint8_t)payload;
    return uart_result(b, b->uart->send(b->uart->context, packet, 5 + payload, deadline(b, ReplyMs)));
}

static int
command(struct boot *b, uint16_t opcode, uint8_t operation, uint8_t sub, uint8_t arg, size_t payload)
{
    uint8_t packet[8] = {0, 0, 0, 0, 0, operation, sub, arg};
    int rc = send_command(b, packet, opcode, payload);
    return rc ? rc : read_reply(b, opcode, operation, sub);
}

static int
switch_baud(struct boot *b)
{
    const struct elden_peach_uart *uart = b->uart;
    int rc = uart_result(b, uart->set_flow(uart->context, false));
    if (rc) {
        return rc;
    }
    uint8_t packet[7] = {0, 0, 0, 0, 0, 2, BaudCode};
    rc = send_command(b, packet, GeneralOpcode, 2);
    if (!rc) {
        uint64_t settle = deadline(b, SwitchMs);
        while (uart->now_ns(uart->context) < settle) {
            uart->wait_until(uart->context, settle);
        }
        rc = uart_result(b, uart->set_baud(uart->context, OperatingBaud));
    }
    int flow = uart->set_flow(uart->context, true);
    if (rc) {
        return rc;
    }
    if (flow) {
        return uart_result(b, flow);
    }
    rc = read_reply(b, GeneralOpcode, 2, 0);
    return rc == 1 ? 0 : (rc < 0 ? rc : ELDEN_PEACH_REJECTED);
}

static int
download(struct boot *b, const struct elden_peach_image *image, uint8_t ssid,
         const struct nvm_offsets *nvm)
{
    uint8_t packet[8 + Chunk] = {0, 0, 0, 0, 0, EdlSegment, ssid, 0};
    for (size_t start = 0; start < image->size;) {
        size_t length = image->size - start;
        if (length > Chunk) {
            length = Chunk;
        }
        for (size_t i = 0; i < length; ++i) {
            size_t offset = start + i;
            uint8_t byte = image->data[offset];
            if (nvm && offset == nvm->baud) {
                byte = BaudCode;
            } else if (nvm && offset == nvm->sleep) {
                byte |= 1;
            }
            packet[8 + i] = byte;
        }
        packet[7] = (uint8_t)length;
        int rc = send_command(b, packet, EdlOpcode, 3 + length);
        if (rc) {
            return rc;
        }
        start += length;
        if (nvm || start == image->size) {
            rc = read_reply(b, EdlOpcode, EdlSegment, ssid);
            if (rc) {
                return rc;
            }
        }
    }
    return 0;
}

static void
expect_notification(struct boot *b, enum notification notification)
{
    b->expected = notification;
    b->notified = false;
}

static int
wait_notification(struct boot *b)
{
    uint64_t end = deadline(b, ReplyMs);
    int rc = 0;
    while (!b->notified && !rc) {
        struct packet packet;
        rc = read_packet(b, &packet, end);
    }
    b->expected = NoNotification;
    return rc;
}

static int
boot_peripheral(struct boot *b, const struct elden_peach_firmware *fw,
                const struct nvm_offsets *offsets)
{
    int rc = command(b, EdlOpcode, EdlArbitrate, PatchSsid, 0, 3);
    if (rc == PeachPatched) {
        return 0;
    }
    if (!rc) {
        rc = download(b, &fw->peripheral, PatchSsid, NULL);
    }
    if (!rc) {
        rc = download(b, &fw->peripheral_nvm, PatchSsid, offsets);
    }
    if (!rc) {
        expect_notification(b, PatchReady);
        rc = command(b, GeneralOpcode, 3, 0, 0, 1);
        if (!rc) {
            rc = wait_notification(b);
        }
    }
    return rc;
}

static int
boot_tme(struct boot *b, const struct elden_peach_image *tme)
{
    int rc = wake(b);
    if (rc) {
        return rc;
    }
    rc = command(b, EdlOpcode, EdlArbitrate, TmeSsid, 0, 3);
    if (rc == 5 || rc == 6) {
        return 0;
    }
    if (rc != 0 && rc != PeachPatched) {
        return rc;
    }
    bool patched = rc == PeachPatched;
    rc = command(b, EdlOpcode, EdlVersion, TmeSsid, 0, 2);
    if (!rc && !patched) {
        expect_notification(b, TmeReady);
        rc = download(b, tme, TmeSsid, NULL);
        if (!rc) {
            rc = wait_notification(b);
        }
    }
    return rc;
}

static int
hci_version(struct boot *b)
{
    const uint8_t request[] = {0x01, 0x00, 0xfc, 0x01, 0x19};
    int rc = uart_result(b, b->uart->send(b->uart->context, request, sizeof(request), deadline(b, ReplyMs)));
    uint64_t end = deadline(b, ReplyMs);
    while (!rc) {
        struct packet packet;
        rc = read_packet(b, &packet, end);
        const uint8_t *data = packet.data;
        if (rc || packet.type != HciEvent || packet.size < 5 ||
            data[0] != 0x0e || le16(data + 3) != 0xfc00) {
            continue;
        }
        if (packet.size != 20 || data[5] != 0 || data[6] != 0x19) {
            return ELDEN_PEACH_BAD_PACKET;
        }
        b->result->product = le32(data + 8);
        b->result->patch = le16(data + 12);
        b->result->rom = le16(data + 14);
        b->result->soc = le32(data + 16);
        if ((b->result->product != 0x21 && b->result->product != 0x1e) ||
            b->result->rom != 0x0200 || b->result->soc != UINT32_C(0x40210200)) {
            return ELDEN_PEACH_WRONG_VERSION;
        }
        return 0;
    }
    return rc;
}

int
elden_peach_boot(const struct elden_peach_uart *uart, const struct elden_peach_firmware *fw,
                struct elden_peach_result *result)
{
    if (!result) {
        return ELDEN_PEACH_BAD_INPUT;
    }
    *result = (struct elden_peach_result){0};
    struct nvm_offsets offsets = {0};
    if (!uart || !uart->now_ns || !uart->send || !uart->receive || !uart->set_baud ||
        !uart->set_flow || !uart->wait_until || !fw || !validate_firmware(fw, &offsets)) {
        return ELDEN_PEACH_BAD_INPUT;
    }
    struct boot b = {uart, result, NoNotification, false};
    result->stage = ELDEN_PEACH_WAKE;
    int rc = wake(&b);
    if (!rc) {
        result->stage = ELDEN_PEACH_PERIPHERAL_VERSION;
        rc = command(&b, EdlOpcode, EdlVersion, PatchSsid, 0, 2);
    }
    if (!rc) {
        result->stage = ELDEN_PEACH_BAUD;
        rc = switch_baud(&b);
    }
    if (!rc) {
        result->stage = ELDEN_PEACH_PERIPHERAL_PATCH;
        rc = boot_peripheral(&b, fw, &offsets);
    }
    if (!rc) {
        result->stage = ELDEN_PEACH_TME;
        rc = boot_tme(&b, &fw->tme);
    }
    if (!rc) {
        result->stage = ELDEN_PEACH_ACTIVATE;
        expect_notification(&b, Activated);
        rc = command(&b, GeneralOpcode, 0, 1, 1, 3);
        if (rc == PeachPatched) {
            rc = 0;
        } else if (!rc) {
            rc = wait_notification(&b);
        }
    }
    if (!rc) {
        result->stage = ELDEN_PEACH_HCI_VERSION;
        rc = hci_version(&b);
    }
    if (!rc) {
        result->stage = ELDEN_PEACH_DONE;
    }
    return rc > 0 ? ELDEN_PEACH_REJECTED : rc;
}
