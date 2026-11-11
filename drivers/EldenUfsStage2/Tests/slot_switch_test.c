/* SPDX-License-Identifier: GPL-3.0-only */
#include "EldenSlotSwitch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BLOCK 4096u
#define SECTORS 64u
#define BACKUP 59u

static uint8_t media[2][SECTORS][BLOCK], original[2][SECTORS][BLOCK];
static struct elden_slot_change change;
static unsigned lun, operations, fail_at;
static bool fail_again;

static void put(uint8_t *where, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) where[i] = (uint8_t)(value >> (8 * i));
}

static void entry(uint8_t *table, unsigned index, const char *name, unsigned flags)
{
    uint8_t *e = table + index * 128;
    e[0] = (uint8_t)(40 + index);
    e[16] = (uint8_t)(60 + index);
    put(e + 32, 6 + 3 * index, 8);
    put(e + 40, 8 + 3 * index, 8);
    e[54] = (uint8_t)flags;
    for (unsigned i = 0; name[i]; ++i) e[56 + 2 * i] = (uint8_t)name[i];
}

static void gpt(unsigned disk, unsigned entries)
{
    unsigned blocks = entries / 32;
    memcpy(media[disk][BACKUP], media[disk][2], blocks * BLOCK);
    for (unsigned copy = 0; copy < 2; ++copy) {
        uint8_t *h = media[disk][copy ? 63 : 1];
        memset(h, 0, BLOCK);
        memcpy(h, "EFI PART", 8);
        put(h + 8, 0x10000, 4);
        put(h + 12, 92, 4);
        put(h + 24, copy ? 63 : 1, 8);
        put(h + 32, copy ? 1 : 63, 8);
        put(h + 40, 6, 8);
        put(h + 48, BACKUP - 1, 8);
        h[56] = (uint8_t)(disk + 1);
        put(h + 72, copy ? BACKUP : 2, 8);
        put(h + 80, entries, 4);
        put(h + 84, 128, 4);
        put(h + 88, kshim_crc32(media[disk][2], entries * 128), 4);
        put(h + 16, kshim_crc32(h, 92), 4);
    }
}

static void reset(void)
{
    memcpy(media, original, sizeof(media));
    operations = fail_at = 0;
    lun = 2;
    fail_again = false;
}

static int read_blocks(void *context, uint64_t lba, uint32_t blocks, void *bytes)
{
    unsigned disk = *(unsigned *)context;
    memcpy(bytes, media[disk][lba], blocks * BLOCK);
    return 0;
}

static int fail_operation(void)
{
    ++operations;
    return fail_at && (operations == fail_at || (fail_again && operations > fail_at));
}

static int write_blocks(void *context, uint64_t lba, uint32_t blocks, const void *bytes)
{
    unsigned disk = *(unsigned *)context;
    unsigned table_blocks = disk ? 3 : 1;
    assert((blocks == 1 && (lba == 1 || lba == 63)) ||
           (blocks == table_blocks && (lba == 2 || lba == BACKUP)));
    // A failing transfer may already have written to storage.
    memcpy(media[disk][lba], bytes, blocks * BLOCK);
    return fail_operation();
}

static int flush(void *context)
{
    (void)context;
    return fail_operation();
}

static int get_lun(void *context, uint32_t *value)
{
    (void)context;
    *value = lun;
    return 0;
}

static int set_lun(void *context, uint32_t value)
{
    (void)context;
    lun = value;
    return fail_operation();
}

int main(void)
{
    unsigned ids[2] = {0, 1};
    struct kshim_blockdev disk[2] = {
        { &ids[0], BLOCK, SECTORS, read_blocks, write_blocks, flush },
        { &ids[1], BLOCK, SECTORS, read_blocks, write_blocks, flush },
    };
    entry(media[0][2], 0, "vbmeta_system_a", 0x0b);
    entry(media[0][2], 1, "vbmeta_system_b", 0x7f);
    entry(media[0][2], 2, "rootfs", 0);
    entry(media[1][2], 0, "boot_a", 0x0b);
    entry(media[1][2], 1, "boot_b", 0x7f);
    entry(media[1][2], 2, "dtbo_a", 0x0b);
    entry(media[1][2], 3, "dtbo_b", 0x7f);
    entry(media[1][2], 4, "unrelated_a", 0x0b);
    entry(media[1][2], 5, "unrelated_b", 0x7f);
    for (unsigned i = 6; i <= 9; i += 3) {
        memcpy(media[1][i], "ANDROID!", 8);
        put(media[1][i] + 8, 1024, 4);
        put(media[1][i] + 20, 1584, 4);
        put(media[1][i] + 40, 4, 4);
    }
    gpt(0, 32);
    gpt(1, 96);
    memcpy(original, media, sizeof(media));
    reset();
    assert(!elden_slot_plan(&change, &disk[0], &disk[1], 2, 1));
    assert(!elden_slot_apply(&change, NULL, get_lun, set_lun) && lun == 1);
    unsigned operation_count = operations;
    assert(media[1][2][0] == 41 && media[1][2][128] == 40);
    assert(media[1][2][54] == 0x0f && media[1][2][128 + 54] == 0x7b);
    assert(!memcmp(media[1][2] + 4 * 128, original[1][2] + 4 * 128, 2 * 128));
    assert(!memcmp(media[1][6], original[1][6], 6 * BLOCK));
    assert(!elden_slot_plan(&change, &disk[0], &disk[1], 1, 2));
    assert(!elden_slot_apply(&change, NULL, get_lun, set_lun) && lun == 2);
    assert(media[1][2][0] == 40 && media[1][2][128] == 41);
    assert(!elden_slot_plan(&change, &disk[0], &disk[1], 2, 0));
    unsigned prior = operations;
    assert(!elden_slot_apply(&change, NULL, get_lun, set_lun) && operations == prior);

    for (unsigned i = 1; i <= operation_count; ++i) {
        reset();
        assert(!elden_slot_plan(&change, &disk[0], &disk[1], 2, 1));
        fail_at = i;
        assert(elden_slot_apply(&change, NULL, get_lun, set_lun) == ELDEN_SLOT_ROLLED_BACK);
        assert(lun == 2 && !memcmp(media, original, sizeof(media)));
    }
    reset();
    assert(!elden_slot_plan(&change, &disk[0], &disk[1], 2, 1));
    fail_at = 1;
    fail_again = true;
    assert(elden_slot_apply(&change, NULL, get_lun, set_lun) == ELDEN_SLOT_UNCERTAIN);

    reset();
    assert(elden_slot_plan(&change, &disk[0], &disk[1], 1, 2) == KSHIM_STORAGE_CONFLICT);
    assert(elden_slot_apply(&change, NULL, get_lun, set_lun) == KSHIM_STORAGE_ARG);
    media[0][BACKUP][0] ^= 1;
    assert(elden_slot_plan(&change, &disk[0], &disk[1], 2, 1) == KSHIM_STORAGE_CRC);
    reset();
    assert(!elden_slot_plan(&change, &disk[0], &disk[1], 2, 1));
    media[1][2][0] ^= 1;
    assert(elden_slot_apply(&change, NULL, get_lun, set_lun) == KSHIM_STORAGE_CRC);
    assert(operations == 0 && lun == 2);
    puts("slot switch: both directions, CRC and write bounds, stale plans, no-op and rollback passed");
    return 0;
}
