// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "EldenNovatekTouch.h"
#include "GeniSpi.h"

#include <string.h>

enum {
    EVENT_ADDRESS = 0x130900,
    RESET_STATE = EVENT_ADDRESS + 0x60,
    FIRMWARE_INFO = EVENT_ADDRESS + 0x78,
    HOST_COMMAND = EVENT_ADDRESS + 0x50,
    TRIM_ID = 0x1fb104,
    BOOTLOADER_RESET = 0x1fb43e,
    ENGINEERING_RESET = 0x7fff80,
    BOOT_READY = 0x1fb50d,
    AUTO_COPY = 0x1fc825,
    ILM_DESTINATION = 0x1fcc30,
    DLM_DESTINATION = 0x1fcc40,
    REPORT_BYTES = 108,
    CHECKSUM_BYTES = 65,
    RECORD_BYTES = 6,
    READ_PREFIX_BYTES = 2,
    STATE_ENTER = 1,
    STATE_MOVE = 2,
    FIRMWARE_SECTOR_BYTES = 4096,
    FIRMWARE_CHUNK_BYTES = ELDEN_TOUCH_FW_SCRATCH_BYTES - 1,
};

struct firmware_part {
    uint32_t offset;
    uint32_t address;
    uint32_t last_byte;
    uint32_t crc;
};

static uint32_t
read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int
parse_firmware(const uint8_t *image, size_t bytes, struct firmware_part parts[4])
{
    if (bytes != ELDEN_TOUCH_FW_BYTES || memcmp(image + bytes - 3, "NVT", 3) != 0 ||
        image[bytes - FIRMWARE_SECTOR_BYTES] + image[bytes - FIRMWARE_SECTOR_BYTES + 1] != 255 ||
        !(image[0x20] & 2) || (image[0x28] & 0x10) || (image[0x29] & 1)) {
        return GENI_ERR_PROTO;
    }

    uint32_t header_bytes = read_le32(image);
    if (header_bytes < 0x30 || header_bytes >= bytes - FIRMWARE_SECTOR_BYTES || header_bytes % 16) {
        return GENI_ERR_PROTO;
    }

    // The Elden image contains two executable banks and one self-copy
    // descriptor in each cascade header.
    for (unsigned i = 0; i < 2; i++) {
        const uint8_t *entry = image + i * 12;
        parts[i] = (struct firmware_part){read_le32(entry), read_le32(entry + 4),
            read_le32(entry + 8), read_le32(image + 0x18 + i * 4)};
    }
    uint32_t first_header_bytes = 0;
    for (uint32_t offset = 0x30; offset < header_bytes; offset += 16) {
        const uint8_t *entry = image + offset;
        if (read_le32(entry + 4) != 0) {
            first_header_bytes = offset + 16;
            parts[2] = (struct firmware_part){read_le32(entry + 8), read_le32(entry),
                read_le32(entry + 4), read_le32(entry + 12)};
            break;
        }
    }
    if (first_header_bytes != 0x100 || first_header_bytes > header_bytes / 2) {
        return GENI_ERR_PROTO;
    }
    const uint8_t *cascade = image + 2 * first_header_bytes - 16;
    parts[3] = (struct firmware_part){read_le32(cascade + 8), read_le32(cascade),
        read_le32(cascade + 4), read_le32(cascade + 12)};
    if (parts[2].offset != 0 || parts[3].offset != first_header_bytes ||
        parts[2].last_byte != first_header_bytes - 1 || parts[3].last_byte != first_header_bytes - 1) {
        return GENI_ERR_PROTO;
    }

    const uint32_t destinations[] = {0, 0x100000, 0x1158c0, 0x1159c0};
    const uint32_t capacities[] = {0x20000, 0x158c0, 0x100, 0x100};
    size_t content_bytes = bytes - FIRMWARE_SECTOR_BYTES;
    for (unsigned i = 0; i < 4; i++) {
        const struct firmware_part *part = &parts[i];
        if (part->address != destinations[i] || part->last_byte == 0 ||
            part->last_byte >= capacities[i] || part->offset >= content_bytes ||
            part->last_byte >= content_bytes - part->offset) {
            return GENI_ERR_PROTO;
        }
    }
    if (parts[1].offset < header_bytes || parts[0].last_byte >= parts[1].offset - header_bytes) {
        return GENI_ERR_PROTO;
    }
    return 0;
}

static int
select_page(struct geni_spi *spi, uint32_t address)
{
    const uint8_t page[] = {0xff, (address >> 15) & 0xff, (address >> 7) & 0xff};
    return geni_spi_transfer(spi, page, NULL, sizeof(page), 0);
}

static int
read_registers(struct geni_spi *spi, uint32_t address, uint8_t *output, size_t bytes)
{
    int error = select_page(spi, address);
    if (error != 0) {
        return error;
    }
    const uint8_t request[REPORT_BYTES + READ_PREFIX_BYTES] = {address & 0x7f};
    uint8_t response[sizeof(request)];
    error = geni_spi_transfer(spi, request, response, bytes + READ_PREFIX_BYTES, 0);
    if (error == 0) {
        memcpy(output, response + READ_PREFIX_BYTES, bytes);
    }
    return error;
}

static int
write_packet(struct geni_spi *spi, uint32_t address, const uint8_t *packet, size_t bytes)
{
    int error = select_page(spi, address);
    return error != 0 ? error : geni_spi_transfer(spi, packet, NULL, bytes, 0);
}

static int
write_register(struct geni_spi *spi, uint32_t address, uint32_t value, unsigned bytes)
{
    uint8_t packet[5] = {(address & 0x7f) | 0x80};
    for (unsigned i = 0; i < bytes; i++) {
        packet[1 + i] = (uint8_t)(value >> (i * 8));
    }
    return write_packet(spi, address, packet, 1 + bytes);
}

static int
bootloader_reset(struct geni_spi *spi)
{
    int error = write_register(spi, BOOTLOADER_RESET, 0x69, 1);
    if (error == 0) {
        spi->se.delay_us(spi->se.ctx, 5000);
    }
    return error;
}

static int
identify_controller(struct geni_spi *spi)
{
    int error = write_register(spi, ENGINEERING_RESET, 0x5a, 1);
    if (error != 0) {
        return error;
    }
    spi->se.delay_us(spi->se.ctx, 11000);
    error = bootloader_reset(spi);
    if (error != 0) {
        return error;
    }
    const uint8_t trim_request[7] = {(TRIM_ID & 0x7f) | 0x80};
    error = write_packet(spi, TRIM_ID, trim_request, sizeof(trim_request));
    if (error != 0) {
        return error;
    }
    uint8_t trim[6];
    error = read_registers(spi, TRIM_ID, trim, sizeof(trim));
    if (error != 0) {
        return error;
    }
    return trim[0] == 0x15 && trim[3] == 0x23 && trim[4] == 0x65 && trim[5] == 0x03 ?
        0 : GENI_ERR_NODEV;
}

static int
program_crc(struct geni_spi *spi, uint32_t bank, const struct firmware_part *part)
{
    int error = write_register(spi, bank, part->address, 3);
    if (error == 0) {
        error = write_register(spi, bank + 4, part->last_byte, 3);
    }
    if (error == 0) {
        error = write_register(spi, bank + 8, part->crc, 4);
    }
    return error;
}

static int
write_firmware_part(struct geni_spi *spi, const uint8_t *image,
    const struct firmware_part *part, uint8_t *packet)
{
    size_t bytes = (size_t)part->last_byte + 1;
    for (size_t position = 0; position < bytes;) {
        size_t chunk = bytes - position;
        if (chunk > FIRMWARE_CHUNK_BYTES) {
            chunk = FIRMWARE_CHUNK_BYTES;
        }
        uint32_t address = part->address + (uint32_t)position;
        packet[0] = (address & 0x7f) | 0x80;
        memcpy(packet + 1, image + part->offset + position, chunk);
        int error = write_packet(spi, address, packet, chunk + 1);
        if (error != 0) {
            return error;
        }
        position += chunk;
    }
    return 0;
}

static int
wait_for_value(struct geni_spi *spi, uint32_t address, uint8_t first, uint8_t last,
    unsigned attempts, unsigned delay_us)
{
    for (unsigned i = 0; i < attempts; i++) {
        spi->se.delay_us(spi->se.ctx, delay_us);
        uint8_t value;
        int error = read_registers(spi, address, &value, 1);
        if (error != 0) {
            return error;
        }
        if (value >= first && value <= last) {
            return 0;
        }
    }
    return GENI_ERR_TIMEOUT;
}

int
elden_touch_upload_firmware(struct geni_spi *spi, const uint8_t *image, size_t image_bytes,
    uint8_t *scratch, size_t scratch_bytes, struct elden_touch_firmware_info *info)
{
    if (spi == NULL || spi->se.delay_us == NULL || image == NULL || info == NULL ||
        scratch == NULL || scratch_bytes < ELDEN_TOUCH_FW_SCRATCH_BYTES ||
        (spi->use_dma && spi->dma.size < ELDEN_TOUCH_FW_SCRATCH_BYTES)) {
        return GENI_ERR_PARAM;
    }
    struct firmware_part parts[4];
    int error = parse_firmware(image, image_bytes, parts);
    if (error == 0) {
        error = identify_controller(spi);
    }
    if (error == 0) {
        error = bootloader_reset(spi);
    }
    if (error == 0) {
        error = program_crc(spi, ILM_DESTINATION, &parts[0]);
    }
    if (error == 0) {
        error = program_crc(spi, DLM_DESTINATION, &parts[1]);
    }
    if (error == 0) {
        error = write_register(spi, AUTO_COPY, 0x56, 1);
    }
    for (unsigned i = 0; error == 0 && i < 4; i++) {
        error = write_firmware_part(spi, image, &parts[i], scratch);
    }
    if (error == 0) {
        error = wait_for_value(spi, AUTO_COPY, 0, 0, 200, 1000);
    }
    if (error == 0) {
        const uint8_t clear_reset[7] = {(RESET_STATE & 0x7f) | 0x80};
        error = write_packet(spi, RESET_STATE, clear_reset, sizeof(clear_reset));
    }
    if (error == 0) {
        error = write_register(spi, HOST_COMMAND, 0xae, 2);
    }
    if (error == 0) {
        error = write_register(spi, BOOT_READY, 1, 1);
    }
    if (error == 0) {
        error = wait_for_value(spi, RESET_STATE, 0xa0, 0xaf, 100, 10000);
    }
    uint8_t firmware_info[38];
    if (error == 0) {
        error = read_registers(spi, FIRMWARE_INFO, firmware_info, sizeof(firmware_info));
    }
    if (error != 0) {
        return error;
    }
    if (firmware_info[0] + firmware_info[1] != 255) {
        return GENI_ERR_PROTO;
    }
    uint16_t product = firmware_info[34] | ((uint16_t)firmware_info[35] << 8);
    if (product != 0x6097 || firmware_info[12] != 0xf1) {
        return GENI_ERR_NODEV;
    }
    *info = (struct elden_touch_firmware_info){product, firmware_info[0], firmware_info[13], firmware_info[12]};
    return 0;
}

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

    uint8_t report[REPORT_BYTES];
    int error = read_registers(spi, EVENT_ADDRESS, report, sizeof(report));
    return error != 0 ? error : decode_report(report, frame);
}
