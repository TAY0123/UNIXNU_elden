/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Sm8750RpmhRead.h"
#include "Sm8750CmdDb.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

enum {
    Id = 0x0000,
    Config = 0x000c,
    IrqEnable = 0x0d00,
    IrqStatus = 0x0d04,
    IrqClear = 0x0d08,
    Wait = 0x0fc0,
    Control = 0x0fc4,
    Status = 0x0fc8,
    Enable = 0x0fcc,
    Message = 0x0fd4,
    Address = 0x0fd8,
    Data = 0x0fdc,
    CommandStatus = 0x0fe0,
    Response = 0x0fe4,
    Slot = 2,
    OtherSlot = 1,
    Trigger = 0x01010000,
    Mx = 0x30010,
    Cx = 0x30000,
};

typedef struct {
    uint32_t registers[0x2000 / 4];
    unsigned writes;
    unsigned polls;
    unsigned transactions;
    unsigned fail_transaction;
    bool marked;
    bool triggered;
    bool keep_busy;
    bool invalid_completion;
    bool refuse_address;
    bool refuse_irq_clear;
    bool rails;
} Model;

static uint32_t
read(void *context, uint32_t offset)
{
    Model *model = context;
    assert(offset % 4 == 0 && offset < sizeof(model->registers));
    return model->registers[offset / 4];
}

static void
write(void *context, uint32_t offset, uint32_t value)
{
    Model *model = context;
    ++model->writes;
    switch (offset) {
    case IrqClear:
        assert(value == Slot && model->triggered);
        if (!model->refuse_irq_clear) {
            model->registers[IrqStatus / 4] &= ~value;
            model->triggered = false;
            model->marked = false;
        }
        return;
    case Control:
        if (value == Trigger) {
            assert(model->marked && !model->triggered);
            assert(model->registers[Message / 4] == (model->rails ? 0x10108u : 0x108u));
            assert(model->registers[Address / 4] == (model->transactions ? Cx : Mx));
            assert(model->registers[Wait / 4] == (unsigned)model->rails);
            if (model->rails) {
                assert(model->registers[Data / 4] == (model->transactions ? 10u : 9u));
            }
            assert(model->registers[Enable / 4] == 1);
            model->triggered = true;
            ++model->transactions;
            model->polls = 0;
            model->registers[Status / 4] = 0;
        }
        break;
    case Address:
        if (model->refuse_address && value == Mx) {
            return;
        }
        break;
    case Message:
    case Enable:
    case Wait:
    case Data:
        break;
    default:
        assert(false);
    }
    model->registers[offset / 4] = value;
}

static void
delay(void *context, unsigned us)
{
    Model *model = context;
    assert(us == 1 || us == 100);
    if (us != 100) {
        return;
    }
    assert(model->triggered);
    ++model->polls;
    bool busy = model->keep_busy || model->transactions == model->fail_transaction;
    if (!busy && model->polls == 2) {
        model->registers[IrqStatus / 4] |= Slot;
    }
    if (!busy && model->polls == 3) {
        model->registers[Status / 4] = 1;
        model->registers[CommandStatus / 4] = model->invalid_completion ? UINT32_MAX : 0x10100;
        model->registers[Response / 4] = 0x1234;
    }
}

static void
mark(void *context)
{
    Model *model = context;
    assert(!model->marked && !model->triggered);
    model->marked = true;
}

static Model
idle(void)
{
    Model model = {0};
    model.registers[Id / 4] = 0x00030800;
    model.registers[Config / 4] = 0x800c8104;
    model.registers[IrqEnable / 4] = OtherSlot;
    model.registers[IrqStatus / 4] = OtherSlot;
    model.registers[Status / 4] = 1;
    model.registers[Message / 4] = 0x10108;
    model.registers[Address / 4] = 0x30000;
    model.registers[Data / 4] = 0x77;
    return model;
}

static void
check_restored(const Model *model)
{
    assert(model->registers[IrqEnable / 4] == OtherSlot);
    assert(model->registers[IrqStatus / 4] == OtherSlot);
    assert(model->registers[Wait / 4] == 0);
    assert(model->registers[Control / 4] == 0);
    assert(model->registers[Enable / 4] == 0);
    assert(model->registers[Message / 4] == 0x10108);
    assert(model->registers[Address / 4] == 0x30000);
    assert(model->registers[Data / 4] == 0x77);
}

static void
check_rails(const char *path, Model *model, const Sm8750RpmhReadIo *io)
{
    static uint8_t db[Sm8750CmdDbSize];
    FILE *file = fopen(path, "rb");
    assert(file && fread(db, 1, sizeof(db), file) == sizeof(db));
    assert(fgetc(file) == EOF && !ferror(file));
    assert(fclose(file) == 0);

    Sm8750RpmhGpuRailResult result;
    *model = idle();
    model->rails = true;
    assert(Sm8750RpmhHoldGpuRails(io, db, sizeof(db), &result) == Sm8750RpmhReadOk);
    assert(result.mx_index == 9 && result.cx_index == 10);
    assert(result.mx.acknowledged && result.cx.acknowledged && model->transactions == 2);
    check_restored(model);

    for (unsigned transaction = 1; transaction <= 2; ++transaction) {
        *model = idle();
        model->rails = true;
        model->fail_transaction = transaction;
        assert(Sm8750RpmhHoldGpuRails(io, db, sizeof(db), &result) == Sm8750RpmhReadTimeout);
        assert(result.mx.triggered && result.mx.acknowledged == (transaction == 2));
        assert(result.cx.triggered == (transaction == 2) && !result.cx.acknowledged);
        assert(model->transactions == transaction && model->registers[Control / 4] == Trigger);
    }

    *model = idle();
    model->rails = true;
    model->refuse_irq_clear = true;
    assert(Sm8750RpmhHoldGpuRails(io, db, sizeof(db), &result) == Sm8750RpmhReadCleanup);
    assert(result.mx.acknowledged && !result.cx.triggered && model->transactions == 1);

    *model = idle();
    assert(Sm8750RpmhHoldGpuRails(io, db, sizeof(db) - 1, &result) == Sm8750RpmhReadArgument);
    assert(!model->writes && !result.mx.triggered && !result.cx.triggered);
}

int
main(int argc, char **argv)
{
    assert(argc == 2);
    Model model = idle();
    Sm8750RpmhReadIo io = {&model, read, write, delay, mark};
    Sm8750RpmhReadResult result;
    assert(Sm8750RpmhReadArc(&io, Mx, &result) == Sm8750RpmhReadOk);
    assert(model.polls == 3 && result.response == 0x1234 && result.irq_status == 3);
    check_restored(&model);

    model = idle();
    assert(Sm8750RpmhReadArc(&io, 0x40010, &result) == Sm8750RpmhReadArgument && !model.writes);
    model.registers[Config / 4] ^= 1;
    assert(Sm8750RpmhReadArc(&io, Mx, &result) == Sm8750RpmhReadHardware && !model.writes);

    model = idle();
    model.registers[IrqEnable / 4] |= Slot;
    assert(Sm8750RpmhReadArc(&io, Mx, &result) == Sm8750RpmhReadBusy && !model.writes);
    model = idle();
    model.registers[Enable / 4] = 2;
    assert(Sm8750RpmhReadArc(&io, Mx, &result) == Sm8750RpmhReadBusy && !model.writes);

    model = idle();
    model.refuse_address = true;
    assert(Sm8750RpmhReadArc(&io, Mx, &result) == Sm8750RpmhReadProgramming);
    assert(result.failed_register == Address && !result.triggered);
    check_restored(&model);

    model = idle();
    model.keep_busy = true;
    assert(Sm8750RpmhReadArc(&io, Mx, &result) == Sm8750RpmhReadTimeout);
    assert(model.polls == 1000 && model.registers[Control / 4] == Trigger);
    assert(model.registers[Enable / 4] == 1 && model.writes == 5);

    model = idle();
    model.invalid_completion = true;
    assert(Sm8750RpmhReadArc(&io, Mx, &result) == Sm8750RpmhReadIncomplete && !result.response);
    check_restored(&model);

    model = idle();
    model.refuse_irq_clear = true;
    assert(Sm8750RpmhReadArc(&io, Mx, &result) == Sm8750RpmhReadCleanup);
    assert(result.failed_register == IrqStatus && model.registers[IrqStatus / 4] == 3);

    check_rails(argv[1], &model, &io);
    puts("Elden RPMh read and ordered GPU rail hold tests passed");
}
