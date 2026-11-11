/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <storage_block.h>

#define ELDEN_SLOT_BLOCK_BYTES 4096u
#define ELDEN_SLOT_TABLE_BYTES 16384u
#define ELDEN_SLOT_DISKS 2u

enum elden_slot_result {
    ELDEN_SLOT_ROLLED_BACK = -30,
    ELDEN_SLOT_UNCERTAIN = -31,
};

struct elden_slot_disk {
    const struct kshim_blockdev *block;
    uint64_t header_lba[2], table_lba[2];
    uint32_t entries, table_blocks;
    uint8_t header[2][ELDEN_SLOT_BLOCK_BYTES];
    uint8_t table[ELDEN_SLOT_TABLE_BYTES], planned[ELDEN_SLOT_TABLE_BYTES];
};

struct elden_slot_change {
    struct elden_slot_disk disks[ELDEN_SLOT_DISKS];
    uint32_t current, target;
    bool ready;
    uint8_t scratch[ELDEN_SLOT_BLOCK_BYTES], readback[ELDEN_SLOT_BLOCK_BYTES];
};

/* Disks are LUN 0 and LUN 4 in that order. Slots 1 and 2 select A and B.
 * Target 0 inspects the current slot without preparing a change. */
int elden_slot_plan(struct elden_slot_change *, const struct kshim_blockdev *lun0,
                    const struct kshim_blockdev *lun4, uint32_t current, uint32_t target);
int elden_slot_apply(struct elden_slot_change *, void *context,
                     int (*get_boot_lun)(void *, uint32_t *),
                     int (*set_boot_lun)(void *, uint32_t));
