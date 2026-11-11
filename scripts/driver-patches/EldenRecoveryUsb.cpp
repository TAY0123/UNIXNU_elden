// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <libkern/c++/OSSharedPtr.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include "EldenUfsIdentity.h"

static uintptr_t
mapPhys(uint64_t address, uint64_t length, OSSharedPtr<IOMemoryMap> &mapping)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(address, length);
    if (!memory) {
        return 0;
    }
    mapping = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    return mapping ? static_cast<uintptr_t>(mapping->getVirtualAddress()) : 0;
}

static uint32_t
read32(uintptr_t base, uint32_t offset)
{
    return *reinterpret_cast<volatile uint32_t *>(base + offset);
}

#define ELDEN_USB_DEBUG 1
#define ELDEN_RECOVERY_USB 1
#include "EldenUsbDebugXnu.inc"

extern "C" void elden_recovery_usb_start(void);

void
elden_recovery_usb_start(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("elden-usb-debug", &enabled, sizeof(enabled)) || enabled != 1) {
        return;
    }
    DTEntry root;
    const void *compatible;
    unsigned int size;
    if (SecureDTLookupEntry(nullptr, "/", &root) != kSuccess ||
        SecureDTGetProperty(root, "compatible", &compatible, &size) != kSuccess ||
        size < sizeof("lenovo,elden") ||
        memcmp(compatible, "lenovo,elden", sizeof("lenovo,elden")) != 0) {
        IOLog("ELDEN_USB: device tree does not identify Elden\n");
        return;
    }
    IOLog("ELDEN_USB: starting Recovery console worker\n");
    eldenStartUsbDebug();
}
