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

#include "EldenPonObserve.inc"

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
#ifdef ELDEN_RECOVERY_RPMH_READ
extern "C" void elden_adreno830_run_rpmh_read(unsigned stage);
static uint64_t gEldenRpmhReadDeadline;
static unsigned gEldenGpuProbeStage;
#endif
#include "EldenUsbDebugXnu.inc"

static unsigned gEldenRecoveryResetInProgress;

static void
elden_recovery_reset(const char *reason)
{
    if (__atomic_exchange_n(&gEldenRecoveryResetInProgress, 1, __ATOMIC_ACQ_REL)) {
        return;
    }
    IOLog("ELDEN_USB: %s; restarting to fastboot\n", reason);
    eldenUsbReset(true);
    IOLog("ELDEN_USB: fastboot reset returned; requesting a normal reset\n");
    eldenUsbReset(false);
    __atomic_store_n(&gEldenRecoveryResetInProgress, 0, __ATOMIC_RELEASE);
}

static void
elden_recovery_usb_timeout(thread_call_param_t, thread_call_param_t)
{
    if (__atomic_load_n(&gEldenUsb.hostSeen, __ATOMIC_ACQUIRE)) {
        return;
    }
    elden_recovery_reset("host connection timed out");
}

static void
elden_recovery_run_timeout(thread_call_param_t, thread_call_param_t)
{
    elden_recovery_reset("Recovery test deadline expired");
}

static uint64_t
elden_recovery_schedule_reset(unsigned seconds, thread_call_func_t callback)
{
    thread_call_t reset = thread_call_allocate_with_options(callback,
        nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    if (!reset) {
        IOLog("ELDEN_USB: unable to schedule the fastboot timeout\n");
        return 0;
    }
    uint64_t deadline;
    clock_interval_to_deadline(seconds, 1000000000, &deadline);
    thread_call_enter_delayed(reset, deadline);
    return deadline;
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
    unsigned management = 0, timeout = 0, runTimeout = 0;
    PE_parse_boot_argn("elden-usb-management", &management, sizeof(management));
    PE_parse_boot_argn("elden-usb-timeout", &timeout, sizeof(timeout));
    PE_parse_boot_argn("elden-usb-run-timeout", &runTimeout, sizeof(runTimeout));
    if (management == 1) {
        if (timeout >= 15 && timeout <= 300) {
            elden_recovery_schedule_reset(timeout, elden_recovery_usb_timeout);
        }
        if (runTimeout >= 15 && runTimeout <= 3600) {
            uint64_t deadline = elden_recovery_schedule_reset(runTimeout, elden_recovery_run_timeout);
#ifdef ELDEN_RECOVERY_RPMH_READ
            unsigned rpmhRead = 0;
            if (PE_parse_boot_argn("elden-adreno830-rpmh-read", &rpmhRead, sizeof(rpmhRead)) &&
                rpmhRead == 1) {
                gEldenRpmhReadDeadline = deadline;
                unsigned rpmhHold = 0, gpuPower = 0;
                gEldenGpuProbeStage = 1;
                if (PE_parse_boot_argn("elden-adreno830-rpmh-hold", &rpmhHold, sizeof(rpmhHold)) &&
                    rpmhHold == 1) {
                    gEldenGpuProbeStage = 2;
                    if (PE_parse_boot_argn("elden-adreno830-power-test", &gpuPower, sizeof(gpuPower)) &&
                        gpuPower == 1) {
                        gEldenGpuProbeStage = 3;
                    }
                }
            }
#else
            (void)deadline;
#endif
        }
    }
    eldenStartUsbDebug();
    eldenPonObserve();
}
