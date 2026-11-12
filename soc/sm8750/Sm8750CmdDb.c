/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 NativeAdreno contributors
 * Adapted from Sm8750CmdDbFind in Shad0w23333/Adreno830 at
 * 1a1321056ffc5428ae925f4680093718b46ca7c2. See LICENSE.Adreno830. */
#include "Sm8750CmdDb.h"

enum {
    DbSlaveSlots = 8,
    DbSlaveHeaderBytes = 16,
    DbDataOffset = 144,
    DbEntryBytes = 24,
    DbNameBytes = 8,
};

static uint32_t
cmd_db_le16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t
cmd_db_le32(const uint8_t *p)
{
    return cmd_db_le16(p) | cmd_db_le16(p + 2) << 16;
}

static int
cmd_db_name_equal(const uint8_t *id, const uint8_t key[DbNameBytes])
{
    for (unsigned i = 0; i < DbNameBytes; ++i) {
        if (id[i] != key[i]) {
            return 0;
        }
    }
    return 1;
}

Sm8750CmdDbStatus
Sm8750CmdDbRead(const void *snapshot, size_t bytes, const char *name, unsigned slave,
                Sm8750CmdDbResource *resource)
{
    const uint8_t *db = snapshot;
    if (!db || bytes != Sm8750CmdDbSize || !name || !resource || !slave || slave > 7 ||
        cmd_db_le32(db + 4) != Sm8750CmdDbMagic) {
        return Sm8750CmdDbInvalid;
    }
    uint8_t key[DbNameBytes] = {0};
    unsigned length = 0;
    while (length < DbNameBytes && name[length]) {
        key[length] = (uint8_t)name[length];
        ++length;
    }
    if (!length || name[length]) {
        return Sm8750CmdDbInvalid;
    }

    Sm8750CmdDbResource found = {0};
    for (unsigned i = 0; i < DbSlaveSlots; ++i) {
        const uint8_t *header = db + 8 + i * DbSlaveHeaderBytes;
        uint32_t id = cmd_db_le16(header);
        if (!id) {
            break;
        }
        uint32_t table = DbDataOffset + cmd_db_le16(header + 2);
        uint32_t data = DbDataOffset + cmd_db_le16(header + 4);
        uint32_t count = cmd_db_le16(header + 6);
        if (table > bytes || count > (bytes - table) / DbEntryBytes || data > bytes) {
            return Sm8750CmdDbInvalid;
        }
        for (uint32_t n = 0; n < count; ++n) {
            const uint8_t *entry = db + table + n * DbEntryBytes;
            uint32_t aux_bytes = cmd_db_le16(entry + 20);
            uint32_t offset = data + cmd_db_le16(entry + 22);
            if (offset > bytes || aux_bytes > bytes - offset) {
                return Sm8750CmdDbInvalid;
            }
            if (!cmd_db_name_equal(entry, key)) {
                continue;
            }
            uint32_t address = cmd_db_le32(entry + 16);
            if (found.address || id != slave || (address >> 16) != slave || (address & 3)) {
                return Sm8750CmdDbInvalid;
            }
            found = (Sm8750CmdDbResource){address, db + offset, aux_bytes};
        }
    }
    if (!found.address) {
        return Sm8750CmdDbMissing;
    }
    *resource = found;
    return Sm8750CmdDbOk;
}
