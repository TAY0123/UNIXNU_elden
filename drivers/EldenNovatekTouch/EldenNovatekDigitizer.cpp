// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <libkern/c++/OSString.h>
#include "EldenNovatekDigitizer.h"
#include "EldenNovatekHid.h"

static_assert(sizeof(IOHIDDevice) == 208, "paired macOS 25F71 IOHIDDevice ABI");

class EldenNovatekDigitizer : public IOHIDDevice {
    OSDeclareDefaultStructors(EldenNovatekDigitizer)

    struct elden_touch_hid state = {};
    IOBufferMemoryDescriptor *input = nullptr;
    struct geni_spi *bus = nullptr;

    static int deliver(void *context, const uint8_t *report, size_t bytes);

public:
    bool handleStart(IOService *provider) override;
    void handleStop(IOService *provider) override;
    void free() override;
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override;
    OSString *newTransportString() const override;
    OSString *newProductString() const override;

    void setBus(struct geni_spi *spi) { bus = spi; }
    int sample();
    int releaseContacts();
};

OSDefineMetaClassAndStructors(EldenNovatekDigitizer, IOHIDDevice)

bool
EldenNovatekDigitizer::handleStart(IOService *provider)
{
    if (!bus || !setProperty("Built-In", true) || !setProperty("DisplayIntegrated", true) ||
        !setProperty("HIDDefaultBehavior", true) || !IOHIDDevice::handleStart(provider)) {
        return false;
    }
    input = IOBufferMemoryDescriptor::withCapacity(ELDEN_TOUCH_HID_REPORT_BYTES, kIODirectionNone);
    if (!input) {
        return false;
    }
    input->setLength(ELDEN_TOUCH_HID_REPORT_BYTES);
    state = {};
    return true;
}

void
EldenNovatekDigitizer::handleStop(IOService *provider)
{
    OSSafeReleaseNULL(input);
    IOHIDDevice::handleStop(provider);
}

void
EldenNovatekDigitizer::free()
{
    OSSafeReleaseNULL(input);
    IOHIDDevice::free();
}

IOReturn
EldenNovatekDigitizer::newReportDescriptor(IOMemoryDescriptor **out) const
{
    if (!out) {
        return kIOReturnBadArgument;
    }
    size_t bytes;
    const uint8_t *descriptor = elden_touch_hid_descriptor(&bytes);
    *out = IOBufferMemoryDescriptor::withBytes(descriptor, bytes, kIODirectionNone);
    return *out ? kIOReturnSuccess : kIOReturnNoMemory;
}

OSString *
EldenNovatekDigitizer::newTransportString() const
{
    return OSString::withCString("SPI");
}

OSString *
EldenNovatekDigitizer::newProductString() const
{
    return OSString::withCString("Elden Touch Screen");
}

int
EldenNovatekDigitizer::deliver(void *context, const uint8_t *report, size_t bytes)
{
    auto *self = static_cast<EldenNovatekDigitizer *>(context);
    if (self->input->writeBytes(0, report, bytes) != bytes) {
        return kIOReturnIOError;
    }
    return self->handleReport(self->input, kIOHIDReportTypeInput, 0);
}

int
EldenNovatekDigitizer::sample()
{
    return input && !isInactive() ? elden_touch_hid_poll(&state, bus, deliver, this) :
        kIOReturnNotAttached;
}

int
EldenNovatekDigitizer::releaseContacts()
{
    const struct elden_touch_frame empty = {};
    return input && !isInactive() ? elden_touch_hid_submit(&state, &empty, deliver, this) :
        kIOReturnNotAttached;
}

IOService *
elden_digitizer_attach(IOService *provider, struct geni_spi *spi)
{
    if (!provider || !spi) {
        return nullptr;
    }
    auto *device = OSTypeAlloc(EldenNovatekDigitizer);
    if (!device) {
        return nullptr;
    }
    if (!device->init() || !device->attach(provider)) {
        device->release();
        return nullptr;
    }
    device->setBus(spi);
    if (!device->start(provider)) {
        device->detach(provider);
        device->release();
        return nullptr;
    }
    return device;
}

int
elden_digitizer_sample(IOService *digitizer)
{
    auto *device = OSDynamicCast(EldenNovatekDigitizer, digitizer);
    return device ? device->sample() : kIOReturnBadArgument;
}

int
elden_digitizer_release_contacts(IOService *digitizer)
{
    auto *device = OSDynamicCast(EldenNovatekDigitizer, digitizer);
    return device ? device->releaseContacts() : kIOReturnBadArgument;
}
