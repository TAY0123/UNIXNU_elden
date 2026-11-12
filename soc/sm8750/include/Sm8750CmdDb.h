/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 NativeAdreno contributors */
#ifndef SM8750_CMD_DB_H
#define SM8750_CMD_DB_H

#include <stddef.h>
#include <stdint.h>

enum {
    Sm8750CmdDbPhysicalBase = 0x81c60000,
    Sm8750CmdDbSize = 0x20000,
    Sm8750CmdDbMagic = 0x0c0330db,
    Sm8750CmdDbArc = 3,
    Sm8750CmdDbBcm = 5,
};

typedef enum {
    Sm8750CmdDbOk,
    Sm8750CmdDbMissing,
    Sm8750CmdDbInvalid,
} Sm8750CmdDbStatus;

typedef struct {
    uint32_t address;
    const uint8_t *aux;
    size_t aux_bytes;
} Sm8750CmdDbResource;

/* The caller owns the immutable, aligned snapshot of the whole reserved region.
 * A missing result means the complete snapshot was valid and had no such name.
 * Any refusal leaves the resource output untouched. */
Sm8750CmdDbStatus Sm8750CmdDbRead(const void *snapshot, size_t bytes, const char *name,
                                 unsigned slave, Sm8750CmdDbResource *resource);

/* Return the address and first hardware index at or above the requested ARC
 * level. Zero padding after the first entry ends the table. */
Sm8750CmdDbStatus Sm8750CmdDbArcLevel(const void *snapshot, size_t bytes, const char *name,
                                      unsigned requested, uint32_t *address, unsigned *index);

/* Return the highest non-padding ARC corner and its firmware level. */
Sm8750CmdDbStatus Sm8750CmdDbArcTop(const void *snapshot, size_t bytes, const char *name,
                                    uint32_t *address, unsigned *index, unsigned *level);

#endif
