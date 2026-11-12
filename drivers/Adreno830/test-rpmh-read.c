/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Sm8750RpmhRead.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

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
    CommandStatus = 0x0fe0,
    Response = 0x0fe4,
    Slot = 2,
    OtherSlot = 1,
    Trigger = 0x01010000,
    Mx = 0x30010,
};

typedef struct {
    uint32_t registers[0x2000 / 4];
    unsigned writes;
    unsigned polls;
    bool marked;
    bool triggered;
    bool keep_busy;
    bool invalid_completion;
    bool refuse_address;
    bool refuse_irq_clear;
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
        }
        return;
    case Control:
        if (value == Trigger) {
            assert(model->marked && !model->triggered);
            assert(model->registers[Message / 4] == 0x108);
            assert(model->registers[Address / 4] == Mx);
            assert(model->registers[Enable / 4] == 1);
            model->triggered = true;
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
    if (!model->keep_busy && model->polls == 2) {
        model->registers[IrqStatus / 4] |= Slot;
    }
    if (!model->keep_busy && model->polls == 3) {
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
}

int
main(void)
{
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
    assert(result.failed_register == Address && !model.marked && !model.triggered);
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
}
