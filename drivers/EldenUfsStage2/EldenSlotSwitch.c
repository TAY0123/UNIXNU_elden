/* SPDX-License-Identifier: GPL-3.0-only */
#include "EldenSlotSwitch.h"
#include <string.h>

#define ENTRY_BYTES 128u
#define HEADER_BYTES 92u
#define SLOT_ACTIVE 0x04u

/* Reviewed partition set used by the TB322FC Linux selector. The XBL LUNs
 * are selected through bBootLunEn and are never written by this operation. */
static const char *const slot_pairs[] = {
    "abl", "aop", "apdp", "cmnlib", "cmnlib64", "devcfg", "dtbo", "hyp",
    "keymaster", "msadp", "qupfw", "storsec", "tz", "vbmeta", "vbmeta_system",
    "boot", "system", "vendor", "modem", "system_ext", "product",
};

static uint32_t read32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t read64(const uint8_t *p)
{
    return read32(p) | (uint64_t)read32(p + 4) << 32;
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) {
        p[i] = (uint8_t)(value >> (i * 8));
    }
}

static bool empty_guid(const uint8_t *p)
{
    static const uint8_t zero[16];
    return memcmp(p, zero, sizeof(zero)) == 0;
}

static int read_header(struct elden_slot_disk *disk, unsigned copy)
{
    uint8_t *header = disk->header[copy];
    uint64_t lba = disk->header_lba[copy];
    int rc = kshim_block_read(disk->block, lba, 1, header);
    if (rc) {
        return rc;
    }
    if (memcmp(header, "EFI PART", 8) || read32(header + 8) != 0x10000 ||
        read32(header + 12) != HEADER_BYTES || read32(header + 20) ||
        read64(header + 24) != lba || read64(header + 32) != disk->header_lba[1 - copy] ||
        empty_guid(header + 56) || read32(header + 84) != ENTRY_BYTES) {
        return KSHIM_STORAGE_FORMAT;
    }
    uint32_t crc = read32(header + 16);
    put32(header + 16, 0);
    bool valid = kshim_crc32(header, HEADER_BYTES) == crc;
    put32(header + 16, crc);
    return valid ? 0 : KSHIM_STORAGE_CRC;
}

static int read_disk(struct elden_slot_disk *disk, const struct kshim_blockdev *block)
{
    if (!block || block->block_size != ELDEN_SLOT_BLOCK_BYTES || block->block_count < 16) {
        return KSHIM_STORAGE_FORMAT;
    }
    disk->block = block;
    disk->header_lba[0] = 1;
    disk->header_lba[1] = block->block_count - 1;
    for (unsigned copy = 0; copy < 2; ++copy) {
        int rc = read_header(disk, copy);
        if (rc) {
            return rc;
        }
        disk->table_lba[copy] = read64(disk->header[copy] + 72);
    }
    const uint8_t *primary = disk->header[0], *backup = disk->header[1];
    uint64_t first = read64(primary + 40), last = read64(primary + 48);
    disk->entries = read32(primary + 80);
    if (memcmp(primary + 40, backup + 40, 32) || memcmp(primary + 80, backup + 80, 12) ||
        !disk->entries || disk->entries > ELDEN_SLOT_TABLE_BYTES / ENTRY_BYTES ||
        first > last || last >= disk->header_lba[1]) {
        return KSHIM_STORAGE_CONFLICT;
    }
    size_t bytes = disk->entries * ENTRY_BYTES;
    disk->table_blocks = (uint32_t)((bytes + ELDEN_SLOT_BLOCK_BYTES - 1) / ELDEN_SLOT_BLOCK_BYTES);
    if (disk->table_lba[0] != 2 || first < 2 + disk->table_blocks ||
        disk->table_lba[1] != last + 1 || disk->table_blocks > disk->header_lba[1] - (last + 1)) {
        return KSHIM_STORAGE_RANGE;
    }
    int rc = kshim_block_read(block, disk->table_lba[0], disk->table_blocks, disk->table);
    if (!rc) {
        rc = kshim_block_read(block, disk->table_lba[1], disk->table_blocks, disk->planned);
    }
    if (rc) {
        return rc;
    }
    if (kshim_crc32(disk->table, bytes) != read32(primary + 88) ||
        memcmp(disk->table, disk->planned, disk->table_blocks * ELDEN_SLOT_BLOCK_BYTES)) {
        return KSHIM_STORAGE_CRC;
    }
    for (unsigned i = 0; i < disk->entries; ++i) {
        const uint8_t *entry = disk->table + i * ENTRY_BYTES;
        if (empty_guid(entry)) {
            continue;
        }
        uint64_t start = read64(entry + 32), end = read64(entry + 40);
        if (empty_guid(entry + 16) || start < first || start > end || end > last) {
            return KSHIM_STORAGE_RANGE;
        }
        for (unsigned j = 0; j < i; ++j) {
            const uint8_t *other = disk->table + j * ENTRY_BYTES;
            if (!empty_guid(other) && (memcmp(entry + 16, other + 16, 16) == 0 ||
                (start <= read64(other + 40) && read64(other + 32) <= end))) {
                return KSHIM_STORAGE_CONFLICT;
            }
        }
    }
    return 0;
}

static unsigned name_slot(const uint8_t *name, const char *base)
{
    while (*base) {
        if (name[0] != (uint8_t)*base || name[1]) {
            return 0;
        }
        name += 2;
        ++base;
    }
    if (name[0] != '_' || name[1] || name[3] || name[4] || name[5]) {
        return 0;
    }
    return name[2] == 'a' ? 1 : name[2] == 'b' ? 2 : 0;
}

static int check_boot(struct elden_slot_change *change, const struct elden_slot_disk *disk,
                      const uint8_t *entry)
{
    uint64_t first = read64(entry + 32), last = read64(entry + 40);
    int rc = kshim_block_read(disk->block, first, 1, change->scratch);
    if (rc) {
        return rc;
    }
    uint32_t version = read32(change->scratch + 40);
    uint32_t bytes = read32(change->scratch + 8);
    if (memcmp(change->scratch, "ANDROID!", 8) || version != 4 ||
        read32(change->scratch + 20) != 1584 || !bytes ||
        ((uint64_t)bytes + ELDEN_SLOT_BLOCK_BYTES - 1) / ELDEN_SLOT_BLOCK_BYTES > last - first) {
        return KSHIM_STORAGE_FORMAT;
    }
    return 0;
}

int elden_slot_plan(struct elden_slot_change *change, const struct kshim_blockdev *lun0,
                    const struct kshim_blockdev *lun4, uint32_t current, uint32_t target)
{
    if (!change || current < 1 || current > 2 || target > 2) {
        return KSHIM_STORAGE_ARG;
    }
    change->ready = false;
    change->current = current;
    change->target = target ? target : current;
    int rc = read_disk(&change->disks[0], lun0);
    if (!rc) {
        rc = read_disk(&change->disks[1], lun4);
    }
    if (rc) {
        return rc;
    }
    for (unsigned pair = 0; pair < sizeof(slot_pairs) / sizeof(slot_pairs[0]); ++pair) {
        const char *name = slot_pairs[pair];
        const uint8_t *entry[2] = {0};
        struct elden_slot_disk *owner = NULL;
        for (unsigned lun = 0; lun < ELDEN_SLOT_DISKS; ++lun) {
            struct elden_slot_disk *disk = &change->disks[lun];
            for (unsigned i = 0; i < disk->entries; ++i) {
                const uint8_t *candidate = disk->table + i * ENTRY_BYTES;
                if (empty_guid(candidate)) {
                    continue;
                }
                unsigned slot = name_slot(candidate + 56, name);
                if (slot) {
                    if (entry[slot - 1] || (owner && owner != disk)) {
                        return KSHIM_STORAGE_CONFLICT;
                    }
                    owner = disk;
                    entry[slot - 1] = candidate;
                }
            }
        }
        bool boot = strcmp(name, "boot") == 0;
        bool dtbo = strcmp(name, "dtbo") == 0;
        bool system_vbmeta = strcmp(name, "vbmeta_system") == 0;
        if ((boot || dtbo) && owner != &change->disks[1]) {
            return KSHIM_STORAGE_NOT_FOUND;
        }
        if (system_vbmeta && owner != &change->disks[0]) {
            return KSHIM_STORAGE_NOT_FOUND;
        }
        if (!owner) {
            continue;
        }
        if (!entry[0] || !entry[1] || !(entry[current - 1][54] & SLOT_ACTIVE) ||
            (entry[2 - current][54] & SLOT_ACTIVE)) {
            return KSHIM_STORAGE_CONFLICT;
        }
        if (change->target == current) {
            continue;
        }
        if (boot) {
            rc = check_boot(change, owner, entry[change->target - 1]);
            if (rc) {
                return rc;
            }
        }
        uint8_t *a = owner->planned + (entry[0] - owner->table);
        uint8_t *b = owner->planned + (entry[1] - owner->table);
        memcpy(a, entry[1], 16);
        memcpy(b, entry[0], 16);
        uint8_t *selected = target == 1 ? a : b;
        uint8_t *other = target == 1 ? b : a;
        selected[54] = 0x0f;
        other[54] &= (uint8_t)~SLOT_ACTIVE;
    }
    change->ready = true;
    return 0;
}

static int verify_copy(struct elden_slot_change *change, const struct elden_slot_disk *disk,
                       unsigned copy, const uint8_t *header, const uint8_t *table)
{
    int rc = kshim_block_read(disk->block, disk->header_lba[copy], 1, change->readback);
    if (rc || memcmp(change->readback, header, ELDEN_SLOT_BLOCK_BYTES)) {
        return rc ? rc : KSHIM_STORAGE_CRC;
    }
    for (unsigned block = 0; block < disk->table_blocks; ++block) {
        rc = kshim_block_read(disk->block, disk->table_lba[copy] + block, 1, change->readback);
        if (rc || memcmp(change->readback, table + block * ELDEN_SLOT_BLOCK_BYTES,
                         ELDEN_SLOT_BLOCK_BYTES)) {
            return rc ? rc : KSHIM_STORAGE_CRC;
        }
    }
    return 0;
}

static int write_copy(struct elden_slot_change *change, const struct elden_slot_disk *disk,
                      unsigned copy, bool restore)
{
    const uint8_t *table = restore ? disk->table : disk->planned;
    uint8_t *header = change->scratch;
    memcpy(header, disk->header[copy], ELDEN_SLOT_BLOCK_BYTES);
    if (!restore) {
        put32(header + 88, kshim_crc32(table, disk->entries * ENTRY_BYTES));
        put32(header + 16, 0);
        put32(header + 16, kshim_crc32(header, HEADER_BYTES));
    }
    int rc = kshim_block_write(disk->block, disk->table_lba[copy], disk->table_blocks, table);
    if (!rc) {
        rc = kshim_block_write(disk->block, disk->header_lba[copy], 1, header);
    }
    if (!rc) {
        rc = kshim_block_flush(disk->block);
    }
    return rc ? rc : verify_copy(change, disk, copy, header, table);
}

int elden_slot_apply(struct elden_slot_change *change, void *context,
                     int (*get_boot_lun)(void *, uint32_t *),
                     int (*set_boot_lun)(void *, uint32_t))
{
    if (!change || !change->ready || !get_boot_lun || !set_boot_lun) {
        return KSHIM_STORAGE_ARG;
    }
    change->ready = false;
    uint32_t boot_lun;
    if (get_boot_lun(context, &boot_lun) || boot_lun != change->current) {
        return KSHIM_STORAGE_CONFLICT;
    }
    if (change->current == change->target) {
        return 0;
    }
    for (unsigned i = 0; i < ELDEN_SLOT_DISKS; ++i) {
        const struct elden_slot_disk *disk = &change->disks[i];
        if (!disk->block->write || !disk->block->flush) {
            return KSHIM_STORAGE_READ_ONLY;
        }
        for (unsigned copy = 0; copy < 2; ++copy) {
            int rc = verify_copy(change, disk, copy, disk->header[copy], disk->table);
            if (rc) {
                return rc;
            }
        }
    }

    /* Backups before primaries. Record each attempted write before issuing it. */
    unsigned touched[ELDEN_SLOT_DISKS] = {0};
    bool lun_attempted = false;
    for (unsigned pass = 0; pass < 2; ++pass) {
        unsigned copy = 1 - pass;
        for (unsigned i = 0; i < ELDEN_SLOT_DISKS; ++i) {
            touched[i] |= 1u << copy;
            if (write_copy(change, &change->disks[i], copy, false)) {
                goto rollback;
            }
        }
    }
    lun_attempted = true;
    if (!set_boot_lun(context, change->target) &&
        !get_boot_lun(context, &boot_lun) && boot_lun == change->target) {
        return 0;
    }

rollback:
    {
        bool restored = true;
        if (lun_attempted && (get_boot_lun(context, &boot_lun) || boot_lun != change->current)) {
            if (set_boot_lun(context, change->current) ||
                get_boot_lun(context, &boot_lun) || boot_lun != change->current) {
                restored = false;
            }
        }
        for (unsigned copy = 0; copy < 2; ++copy) {
            for (unsigned i = 0; i < ELDEN_SLOT_DISKS; ++i) {
                if ((touched[i] & (1u << copy)) && write_copy(change, &change->disks[i], copy, true)) {
                    restored = false;
                }
            }
        }
        return restored ? ELDEN_SLOT_ROLLED_BACK : ELDEN_SLOT_UNCERTAIN;
    }
}
