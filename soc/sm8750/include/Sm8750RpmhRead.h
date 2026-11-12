/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SM8750_RPMH_READ_H
#define SM8750_RPMH_READ_H

#include <stdint.h>

typedef struct {
    void *context;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    void (*delay)(void *, unsigned);
    void (*triggering)(void *);
} Sm8750RpmhReadIo;

typedef enum {
    Sm8750RpmhReadOk,
    Sm8750RpmhReadArgument,
    Sm8750RpmhReadHardware,
    Sm8750RpmhReadBusy,
    Sm8750RpmhReadProgramming,
    Sm8750RpmhReadTimeout,
    Sm8750RpmhReadIncomplete,
    Sm8750RpmhReadCleanup,
} Sm8750RpmhReadStatus;

typedef struct {
    uint32_t hardware;
    uint32_t config;
    uint32_t failed_register;
    uint32_t irq_status;
    uint32_t command_status;
    uint32_t response;
} Sm8750RpmhReadResult;

/* Requires exclusive AP use of the qualified DRV-2 ACTIVE TCS1. A timeout
 * retains the programmed slot until reset. The response is valid on success. */
Sm8750RpmhReadStatus Sm8750RpmhReadArc(const Sm8750RpmhReadIo *io, uint32_t address,
                                     Sm8750RpmhReadResult *result);

#endif
