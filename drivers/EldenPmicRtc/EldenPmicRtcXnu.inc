// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <pexpert/device_tree.h>
#include "EldenPmicRtc.h"
#include "EldenPonResources.h"

namespace {

struct EldenRtcBus {
    uintptr_t map, config;
    uint32_t core(unsigned offset) { return *reinterpret_cast<volatile uint32_t *>(map + offset); }
    uint32_t owner(unsigned offset) { return *reinterpret_cast<volatile uint32_t *>(config + offset); }
};

struct EldenRtcObserver {
    uintptr_t base;
    uint32_t load(unsigned offset) { return *reinterpret_cast<volatile uint32_t *>(base + offset); }
    void command(unsigned offset, uint32_t value)
    {
        *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    void delay() { IODelay(1); }
};

struct EldenRtcState {
    IOMemoryMap *mapping;
    IOLock *lock;
    unsigned rtc;
    int sdam;
};

EldenRtcState eldenRtc;

OSSharedPtr<IOMemoryMap>
eldenRtcMap(uint64_t address, uint64_t length)
{
#if defined(IOKIT_ENABLE_SHARED_PTR)
    OSSharedPtr<IODeviceMemory> memory = IODeviceMemory::withRange(address, length);
    return memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
#else
    OSSharedPtr<IODeviceMemory> memory(IODeviceMemory::withRange(address, length), OSNoRetain);
    return memory ? OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain) : nullptr;
#endif
}

bool
eldenRtcRead(uint32_t &seconds)
{
    if (!__atomic_load_n(&eldenRtc.mapping, __ATOMIC_ACQUIRE)) return false;
    IOLockLock(eldenRtc.lock);
    EldenRtcObserver observer = { static_cast<uintptr_t>(eldenRtc.mapping->getVirtualAddress()) };
    bool valid = elden_rtc::utc(observer, eldenRtc.rtc, eldenRtc.sdam, seconds);
    IOLockUnlock(eldenRtc.lock);
    return valid;
}

} // namespace

extern "C" bool eldenPmicRtcInitialize();

bool
eldenPmicRtcInitialize()
{
    unsigned enabled = 0, useSdam = 0;
    DTEntry root;
    const void *compatible;
    unsigned size;
    if (!PE_parse_boot_argn("elden-pmic-rtc", &enabled, sizeof(enabled)) || enabled != 1 ||
        SecureDTLookupEntry(nullptr, "/", &root) != kSuccess ||
        SecureDTGetProperty(root, "compatible", &compatible, &size) != kSuccess ||
        size < sizeof("lenovo,elden") || memcmp(compatible, "lenovo,elden", sizeof("lenovo,elden"))) return false;

    if (__atomic_load_n(&eldenRtc.mapping, __ATOMIC_ACQUIRE)) return true;
    auto core = eldenRtcMap(ELDEN_SPMI_CORE_PHYS, ELDEN_SPMI_CORE_BYTES);
    auto config = eldenRtcMap(ELDEN_SPMI_CONFIG_PHYS, ELDEN_SPMI_CONFIG_BYTES);
    if (!core || !config) return false;
    EldenRtcBus bus = { static_cast<uintptr_t>(core->getVirtualAddress()),
        static_cast<uintptr_t>(config->getVirtualAddress()) };
    const int rtc = elden_rtc::observerChannel(bus, ELDEN_RTC_PPID);
    PE_parse_boot_argn("elden-rtc-sdam", &useSdam, sizeof(useSdam));
    const int sdam = useSdam == 1 ? elden_rtc::observerChannel(bus, ELDEN_RTC_SDAM_PPID) : -1;
    if (rtc < 0 || (useSdam == 1 && sdam < 0)) {
        IOLog("ELDEN_RTC: no unique observer channel for the clock source\n");
        return false;
    }

    auto mapping = eldenRtcMap(ELDEN_SPMI_OBSERVER_PHYS, ELDEN_SPMI_OBSERVER_BYTES);
    if (!mapping) return false;
    EldenRtcObserver observer = { static_cast<uintptr_t>(mapping->getVirtualAddress()) };
    uint32_t seconds;
    if (!elden_rtc::utc(observer, static_cast<unsigned>(rtc), sdam, seconds)) {
        IOLog("ELDEN_RTC: PMIC did not supply valid UTC\n");
        return false;
    }
    eldenRtc.lock = IOLockAlloc();
    if (!eldenRtc.lock) return false;
    eldenRtc.rtc = static_cast<unsigned>(rtc);
    eldenRtc.sdam = sdam;
    __atomic_store_n(&eldenRtc.mapping, mapping.detach(), __ATOMIC_RELEASE);
    IOLog("ELDEN_RTC: UTC=%u APID=%d SDAM=%d\n", seconds, rtc, sdam);
    return true;
}
