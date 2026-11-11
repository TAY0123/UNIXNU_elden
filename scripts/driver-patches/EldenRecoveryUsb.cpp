// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
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

static bool
eldenRecoveryIsElden()
{
    DTEntry root;
    const void *compatible;
    unsigned int size;
    return SecureDTLookupEntry(nullptr, "/", &root) == kSuccess &&
        SecureDTGetProperty(root, "compatible", &compatible, &size) == kSuccess &&
        size >= sizeof("lenovo,elden") &&
        memcmp(compatible, "lenovo,elden", sizeof("lenovo,elden")) == 0;
}

static unsigned gEldenPanicResetArmed;

extern "C" void elden_recovery_panic_arm(void);

void
elden_recovery_panic_arm(void)
{
    unsigned management = 0;
    if (PE_parse_boot_argn("elden-usb-management", &management, sizeof(management)) &&
        management == 1 && eldenRecoveryIsElden()) {
        __atomic_store_n(&gEldenPanicResetArmed, 1, __ATOMIC_RELEASE);
    }
}

#define ELDEN_USB_DEBUG 1
#define ELDEN_RECOVERY_USB 1
extern "C" void sheng_log_status(const char *message);
#include "EldenUsbDebugXnu.inc"

static void
elden_recovery_usb_timeout(thread_call_param_t, thread_call_param_t)
{
    if (__atomic_load_n(&gEldenUsb.hostSeen, __ATOMIC_ACQUIRE)) {
        return;
    }
    IOLog("ELDEN_USB: host connection timed out; restarting to fastboot\n");
    eldenUsbReset(true);
    IOLog("ELDEN_USB: fastboot reset returned; requesting a normal reset\n");
    eldenUsbReset(false);
}

extern "C" void elden_recovery_usb_start(void);

void
elden_recovery_usb_start(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("elden-usb-debug", &enabled, sizeof(enabled)) || enabled != 1) {
        return;
    }
    if (!eldenRecoveryIsElden()) {
        IOLog("ELDEN_USB: device tree does not identify Elden\n");
        return;
    }
    IOLog("ELDEN_USB: starting Recovery console worker\n");
    unsigned management = 0, timeout = 0;
    PE_parse_boot_argn("elden-usb-management", &management, sizeof(management));
    PE_parse_boot_argn("elden-usb-timeout", &timeout, sizeof(timeout));
    if (management == 1 && timeout >= 15 && timeout <= 300) {
        thread_call_t reset = thread_call_allocate_with_options(elden_recovery_usb_timeout,
            nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
        if (reset) {
            uint64_t deadline;
            clock_interval_to_deadline(timeout, 1000000000, &deadline);
            thread_call_enter_delayed(reset, deadline);
        } else {
            IOLog("ELDEN_USB: unable to schedule the fastboot timeout\n");
        }
    }
    eldenStartUsbDebug();
}
