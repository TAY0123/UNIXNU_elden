// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSData.h>
#include <libkern/crypto/sha2.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include "EldenNovatekDigitizer.h"
#include "EldenNovatekHid.h"
#include "EldenNovatekSpi4.h"
#include "Sm8750Qup1Resources.h"

namespace {

// Limit mappings to the registers used by the SPI4 and GPIO162 paths.
constexpr unsigned GccBytes = 0x53000, TlmmBytes = 0xa3000;
constexpr unsigned QupBytes = 0x2000, EngineBytes = 0x4000;
constexpr unsigned PollMs = 8;
constexpr uint8_t FirmwareDigest[32] = {
    0x41, 0x32, 0x16, 0xd0, 0xe8, 0x73, 0x80, 0x78,
    0x41, 0x4f, 0xa6, 0x81, 0x26, 0x24, 0x39, 0xab,
    0x6d, 0x1c, 0x18, 0x1f, 0x02, 0x94, 0x38, 0x59,
    0x24, 0x74, 0x15, 0xe0, 0x39, 0x4f, 0x10, 0x2f,
};

class RegisterWindow {
    IOMemoryDescriptor *descriptor = nullptr;
    IOMemoryMap *mapping = nullptr;
    volatile uint32_t *registers = nullptr;

    static uint32_t read(void *context, uint32_t offset)
    {
        uint32_t value = static_cast<RegisterWindow *>(context)->registers[offset / 4];
        __asm__ volatile("dmb oshld" ::: "memory");
        return value;
    }

    static void write(void *context, uint32_t offset, uint32_t value)
    {
        __asm__ volatile("dmb oshst" ::: "memory");
        static_cast<RegisterWindow *>(context)->registers[offset / 4] = value;
        __asm__ volatile("dsb oshst" ::: "memory");
    }

    static void delay(void *, unsigned usec)
    {
        if (usec >= 1000) {
            IOSleep((usec + 999) / 1000);
        } else {
            IODelay(usec);
        }
    }

public:
    bool open(uint64_t address, unsigned size)
    {
        descriptor = IOMemoryDescriptor::withPhysicalAddress(address, size, kIODirectionInOut);
        mapping = descriptor ? descriptor->createMappingInTask(kernel_task, 0,
            kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        registers = mapping ? reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress()) : nullptr;
        return registers != nullptr;
    }

    void close()
    {
        registers = nullptr;
        OSSafeReleaseNULL(mapping);
        OSSafeReleaseNULL(descriptor);
    }

    struct geni_io io() { return {this, read, write, delay}; }
};

bool
onElden()
{
    DTEntry root;
    const void *value;
    unsigned int size;
    return SecureDTLookupEntry(nullptr, "/", &root) == kSuccess &&
        SecureDTGetProperty(root, "compatible", &value, &size) == kSuccess &&
        size >= sizeof("lenovo,elden") &&
        memcmp(value, "lenovo,elden", sizeof("lenovo,elden")) == 0;
}

} // namespace

class EldenNovatekController : public IOService {
    OSDeclareDefaultStructors(EldenNovatekController)

    RegisterWindow gcc, tlmm, qup, engine;
    struct elden_touch_spi4 bus = {};
    OSData *firmware = nullptr;
    IOWorkLoop *workLoop = nullptr;
    IOTimerEventSource *timer = nullptr;
    IOService *digitizer = nullptr;

    int initializeController();
    void closeController();
    static void poll(OSObject *owner, IOTimerEventSource *timer);

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
};

OSDefineMetaClassAndStructors(EldenNovatekController, IOService)

bool
EldenNovatekController::start(IOService *provider)
{
    uint32_t enable = 0;
    if (!PE_parse_boot_argn("elden-touch", &enable, sizeof(enable)) || enable != 1 || !onElden()) {
        return false;
    }
    auto *image = OSDynamicCast(OSData, provider->getProperty("novatek,firmware-image"));
    if (!image || image->getLength() != ELDEN_TOUCH_FW_BYTES || !IOService::start(provider)) {
        return false;
    }
    workLoop = IOWorkLoop::workLoop();
    timer = IOTimerEventSource::timerEventSource(this, poll);
    if (!workLoop || !timer || workLoop->addEventSource(timer) != kIOReturnSuccess) {
        OSSafeReleaseNULL(timer);
        OSSafeReleaseNULL(workLoop);
        IOService::stop(provider);
        return false;
    }
    firmware = image;
    firmware->retain();
    if (timer->setTimeoutMS(1) != kIOReturnSuccess) {
        stop(provider);
        return false;
    }
    return true;
}

int
EldenNovatekController::initializeController()
{
    auto *image = static_cast<const uint8_t *>(firmware->getBytesNoCopy());
    SHA256_CTX hash;
    uint8_t digest[sizeof(FirmwareDigest)];
    SHA256_Init(&hash);
    SHA256_Update(&hash, image, ELDEN_TOUCH_FW_BYTES);
    SHA256_Final(digest, &hash);
    if (memcmp(digest, FirmwareDigest, sizeof(digest))) {
        return GENI_ERR_PARAM;
    }
    if (!gcc.open(SM8750_GCC_BASE, GccBytes) || !tlmm.open(SM8750_TLMM_BASE, TlmmBytes) ||
        !qup.open(SM8750_QUP1_BASE, QupBytes) || !engine.open(SM8750_QUP1_SE4_BASE, EngineBytes)) {
        return GENI_ERR_IO;
    }
    bus.gcc = gcc.io();
    bus.tlmm = tlmm.io();
    bus.spi.se = engine.io();
    bus.spi.wrapper = qup.io();
    int result = elden_touch_spi4_start(&bus);
    if (result) {
        return result;
    }
    auto *scratch = static_cast<uint8_t *>(IOMallocData(ELDEN_TOUCH_FW_SCRATCH_BYTES));
    if (!scratch) {
        return GENI_ERR_IO;
    }
    struct elden_touch_firmware_info info = {};
    result = elden_touch_upload_firmware(&bus.spi, image, ELDEN_TOUCH_FW_BYTES,
        scratch, ELDEN_TOUCH_FW_SCRATCH_BYTES, &info);
    IOFreeData(scratch, ELDEN_TOUCH_FW_SCRATCH_BYTES);
    if (!result) {
        result = elden_touch_spi4_arm_events(&bus);
    }
    if (result) {
        return result;
    }
    digitizer = elden_digitizer_attach(this, &bus);
    if (!digitizer) {
        return GENI_ERR_IO;
    }
    IOLog("EldenNovatekController: NT36536 product %04x firmware %02x ready\n", info.product, info.version);
    return 0;
}

void
EldenNovatekController::closeController()
{
    if (digitizer) {
        elden_digitizer_release_contacts(digitizer);
        digitizer->terminate(kIOServiceSynchronous);
        OSSafeReleaseNULL(digitizer);
    }
    int result = elden_touch_spi4_stop(&bus);
    if (result) {
        IOLog("EldenNovatekController: SPI4 restore failed %d\n", result);
        return;
    }
    engine.close();
    qup.close();
    tlmm.close();
    gcc.close();
}

void
EldenNovatekController::poll(OSObject *owner, IOTimerEventSource *source)
{
    auto *self = static_cast<EldenNovatekController *>(owner);
    if (self->firmware) {
        IOLog("EldenNovatekController: initializing SPI4 and NT36536 firmware\n");
        int result = self->initializeController();
        OSSafeReleaseNULL(self->firmware);
        if (result) {
            IOLog("EldenNovatekController: initialization failed %d\n", result);
            self->closeController();
            return;
        }
    }
    int result = elden_digitizer_sample(self->digitizer);
    if (result == ELDEN_TOUCH_SPI4_BUSY) {
        IOLog("EldenNovatekController: GPIO 162 event ownership changed\n");
        self->closeController();
    } else if (source->setTimeoutMS(PollMs) != kIOReturnSuccess) {
        IOLog("EldenNovatekController: touch polling could not be scheduled\n");
        self->closeController();
    }
}

void
EldenNovatekController::stop(IOService *provider)
{
    timer->cancelTimeout();
    workLoop->removeEventSource(timer);
    OSSafeReleaseNULL(timer);
    OSSafeReleaseNULL(workLoop);
    OSSafeReleaseNULL(firmware);
    closeController();
    IOService::stop(provider);
}

void
EldenNovatekController::free()
{
    engine.close();
    qup.close();
    tlmm.close();
    gcc.close();
    IOService::free();
}
