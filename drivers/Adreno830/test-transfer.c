/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "EldenAdreno830.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Offsets relative to the saved tablet DT's GMU register aperture. */
enum { Itcm = 0x36000, Dtcm = 0x3a000, Reset = 0x47000, TcmBytes = 0x4000 };

typedef struct {
    uint32_t itcm[TcmBytes / 4];
    uint32_t dtcm[TcmBytes / 4];
    uint32_t reset;
    unsigned reset_reads, tcm_reads, writes, syncs, barriers;
    unsigned fail_write;
    int fail_read, fail_sync, fail_barrier, corrupt_readback, lose_reset;
    void *synced_cpu;
    size_t synced_bytes;
} RegisterModel;

static uint32_t *register_word(RegisterModel *model, uint32_t offset)
{
    assert(!(offset & 3));
    if (offset >= Itcm && offset < Itcm + TcmBytes) return &model->itcm[(offset - Itcm) / 4];
    assert(offset >= Dtcm && offset < Dtcm + TcmBytes);
    return &model->dtcm[(offset - Dtcm) / 4];
}

static int read32(void *context, uint32_t offset, uint32_t *value)
{
    RegisterModel *model = context;
    if (offset == Reset) {
        ++model->reset_reads;
        if (model->fail_read) return -1;
        *value = model->lose_reset && model->reset_reads == 2 ? 0 : model->reset;
        return 0;
    }
    assert(model->barriers == 1 && model->reset_reads == 1);
    ++model->tcm_reads;
    *value = *register_word(model, offset) ^ (uint32_t)model->corrupt_readback;
    return 0;
}

static int write32(void *context, uint32_t offset, uint32_t value)
{
    RegisterModel *model = context;
    assert(model->reset == 1 && model->reset_reads == 1 && model->barriers == 0);
    uint32_t *word = register_word(model, offset);
    if (++model->writes == model->fail_write) return -1;
    *word = value;
    return 0;
}

static int sync_for_device(void *context, void *cpu, size_t bytes)
{
    RegisterModel *model = context;
    assert(model->reset == 1 && model->reset_reads == 1 && model->barriers == 0);
    ++model->syncs;
    model->synced_cpu = cpu;
    model->synced_bytes = bytes;
    return model->fail_sync;
}

static int barrier(void *context)
{
    RegisterModel *model = context;
    assert(model->reset == 1 && model->reset_reads == 1 && model->tcm_reads == 0);
    ++model->barriers;
    return model->fail_barrier;
}

static void assert_no_io(const RegisterModel *model)
{
    assert(!model->reset_reads && !model->writes && !model->syncs && !model->barriers);
}

int main(int argc, char **argv)
{
    static _Alignas(4096) uint8_t image[88896], icache[0x16000], scratch[4096];
    static RegisterModel model;
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb");
    assert(file && fread(image, 1, sizeof(image), file) == sizeof(image));
    assert(fgetc(file) == EOF && !ferror(file));
    assert(!fclose(file));

    EldenGmuTransfer io = {
        .context = &model, .mmio_bytes = 0x68000,
        .read32 = read32, .write32 = write32, .sync_for_device = sync_for_device, .barrier = barrier,
        .icache = {icache, sizeof(icache), 0x4000},
    };
    const uint32_t chip = ELDEN_ADRENO830_CHIP_ID;
    const size_t tcm_words = (0x1ec + 0x28 + 0x18) / 4;
    model.reset = 1;
    memset(icache, 0xa5, sizeof(icache));
    assert(EldenAdreno830TransferGmu(chip, image, sizeof(image), &io) == EldenGmuSuccess);
    assert(model.writes == tcm_words && model.tcm_reads == tcm_words && model.reset_reads == 2);
    assert(model.barriers == 1 && model.syncs == 1 && model.reset == 1);
    assert(model.synced_cpu == icache && model.synced_bytes == 0x15824);
    assert(!memcmp(model.itcm, image + 0xc0, 0x1ec) && model.itcm[0x1ec / 4] == 0);
    assert(!memcmp(icache, image + 0x2bc, 0x15824) && icache[0x15824] == 0xa5);
    assert(!memcmp(model.dtcm + 0x3f98 / 4, image + 0x15af0, 0x28));
    assert(!memcmp(model.dtcm + 0x3fdc / 4, image + 0x15b28, 0x18));
    assert(model.dtcm[0x3f94 / 4] == 0 && model.dtcm[0x3fc0 / 4] == 0 && model.dtcm[0x3ff4 / 4] == 0);

    model = (RegisterModel){.reset = 1};
    assert(EldenAdreno830TransferGmu(chip, image, sizeof(image) - 1, &io) == EldenGmuInvalidImage);
    assert(EldenAdreno830TransferGmu(0, image, sizeof(image), &io) == EldenGmuInvalidImage);
    io.mmio_bytes -= 4096;
    assert(EldenAdreno830TransferGmu(chip, image, sizeof(image), &io) == EldenGmuInvalidTransfer);
    io.mmio_bytes += 4096;
    io.icache.bytes -= 4096;
    assert(EldenAdreno830TransferGmu(chip, image, sizeof(image), &io) == EldenGmuInvalidTransfer);
    io.icache.bytes += 4096;
    io.icache.iova += 4096;
    assert(EldenAdreno830TransferGmu(chip, image, sizeof(image), &io) == EldenGmuInvalidTransfer);
    io.icache.iova -= 4096;
    io.icache.cpu = image;
    assert(EldenAdreno830TransferGmu(chip, image, sizeof(image), &io) == EldenGmuInvalidTransfer);
    io.icache.cpu = icache;
    io.scratch = (EldenGmuBuffer){icache, 4096, 0x60000000};
    assert(EldenAdreno830TransferGmu(chip, image, sizeof(image), &io) == EldenGmuInvalidTransfer);
    io.scratch = (EldenGmuBuffer){0};
    assert_no_io(&model);

    memset(icache, 0, sizeof(icache));
    model.reset = 0;
    assert(EldenAdreno830TransferGmu(chip, image, sizeof(image), &io) == EldenGmuResetNotAsserted);
    assert(model.reset_reads == 1 && !model.writes && !model.syncs && !icache[0]);
    model = (RegisterModel){.reset = 1, .fail_read = 1};
    assert(EldenAdreno830TransferGmu(chip, image, sizeof(image), &io) == EldenGmuIoFailed);
    assert(model.reset_reads == 1 && !model.writes && !model.syncs);
    model = (RegisterModel){.reset = 1, .fail_write = 2};
    assert(EldenAdreno830TransferGmu(chip, image, sizeof(image), &io) == EldenGmuIoFailed);
    assert(model.writes == 2 && !model.syncs && !model.barriers);
    model = (RegisterModel){.reset = 1, .fail_sync = 1};
    assert(EldenAdreno830TransferGmu(chip, image, sizeof(image), &io) == EldenGmuIoFailed);
    assert(model.writes == 0x1ec / 4 && model.syncs == 1 && !model.barriers);
    model = (RegisterModel){.reset = 1, .fail_barrier = 1};
    assert(EldenAdreno830TransferGmu(chip, image, sizeof(image), &io) == EldenGmuIoFailed);
    assert(model.writes == tcm_words && model.barriers == 1 && !model.tcm_reads);
    model = (RegisterModel){.reset = 1, .corrupt_readback = 1};
    assert(EldenAdreno830TransferGmu(chip, image, sizeof(image), &io) == EldenGmuTcmMismatch);
    assert(model.writes == tcm_words && model.tcm_reads == 1 && model.reset_reads == 1);
    model = (RegisterModel){.reset = 1, .lose_reset = 1};
    assert(EldenAdreno830TransferGmu(chip, image, sizeof(image), &io) == EldenGmuResetNotAsserted);
    assert(model.tcm_reads == tcm_words && model.reset_reads == 2);

    /* The pinned image has no scratch payload. Exercise that window with a
     * second, structurally valid image and require its backing before I/O. */
    static const uint8_t scratch_image[] = {
        0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x78, 0x56, 0x34, 0x12,
        8, 0, 0, 0x60, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xef, 0xcd, 0xab, 0x90,
    };
    model = (RegisterModel){.reset = 1};
    assert(EldenAdreno830TransferGmu(chip, scratch_image, sizeof(scratch_image), &io) == EldenGmuInvalidTransfer);
    assert_no_io(&model);
    io.icache = (EldenGmuBuffer){0};
    io.scratch = (EldenGmuBuffer){scratch, sizeof(scratch), 0x60000000};
    assert(EldenAdreno830TransferGmu(chip, scratch_image, sizeof(scratch_image), &io) == EldenGmuSuccess);
    assert(model.itcm[0] == 0x12345678 && model.writes == 1 && model.tcm_reads == 1);
    assert(model.synced_cpu == scratch + 8 && model.synced_bytes == 4);
    assert(!memcmp(scratch + 8, scratch_image + 36, 4) && !scratch[7] && !scratch[12]);
    puts("Adreno 830 reset-held GMU transfer and refusal cases passed");
}
