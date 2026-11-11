// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "EldenPonResources.h"

static int
eldenPonRead(uintptr_t channel, uint8_t address)
{
    constexpr uint32_t extendedRead = 1U << 27;
    constexpr uint32_t done = 1U;
    constexpr uint32_t errors = 0x0eU;
    *reinterpret_cast<volatile uint32_t *>(channel) =
        extendedRead | (static_cast<uint32_t>(address) << 4);
    for (unsigned retry = 0; retry < 1000; ++retry) {
        const uint32_t status = read32(channel, 0x08);
        if (status & done) {
            return (status & errors) == 0 ? static_cast<int>(read32(channel, 0x18) & 0xff) : -1;
        }
        IODelay(1);
    }
    return -1;
}

static void
eldenPonObserve()
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("elden-pon-observe", &enabled, sizeof(enabled)) || enabled != 1) {
        return;
    }

    OSSharedPtr<IOMemoryMap> coreMapping, configMapping, observerMapping;
    const auto core = mapPhys(ELDEN_SPMI_CORE_PHYS, ELDEN_SPMI_CORE_BYTES, coreMapping);
    const auto config = mapPhys(ELDEN_SPMI_CONFIG_PHYS, ELDEN_SPMI_CONFIG_BYTES, configMapping);
    if (!core || !config) {
        IOLog("ELDEN_PON: failed to map PMIC arbiter\n");
        return;
    }
    const uint32_t version = read32(core, 0);
    if (version < 0x70000000 || version >= 0x80000000) {
        IOLog("ELDEN_PON: unsupported PMIC arbiter version 0x%x\n", version);
        return;
    }
    const unsigned count = read32(core, 4) & 0x7ff;
    if (count == 0 || count > 1024) {
        IOLog("ELDEN_PON: invalid bus 0 channel count %u\n", count);
        return;
    }
    int channel = -1;
    for (unsigned apid = 0; apid < count; ++apid) {
        const unsigned ppid = (read32(core, 0x2000 + apid * 4) >> 8) & 0xfff;
        const unsigned owner = read32(config, apid * 4) & 7;
        if (ppid == ELDEN_PON_PPID && owner == 0) {
            if (channel >= 0) {
                IOLog("ELDEN_PON: ambiguous application channel for the watchdog\n");
                return;
            }
            channel = static_cast<int>(apid);
        }
    }
    if (channel < 0) {
        IOLog("ELDEN_PON: no application channel for the watchdog\n");
        return;
    }

    const auto observer = mapPhys(ELDEN_SPMI_OBSERVER_PHYS, ELDEN_SPMI_OBSERVER_BYTES,
        observerMapping);
    if (!observer) {
        IOLog("ELDEN_PON: failed to map PMIC observer channels\n");
        return;
    }
    const auto pon = observer + static_cast<unsigned>(channel) * 0x20;
    const int reason = eldenPonRead(pon, ELDEN_PON_POFF_REASON);
    const int s1 = eldenPonRead(pon, ELDEN_PON_WATCHDOG_S1);
    const int s2 = eldenPonRead(pon, ELDEN_PON_WATCHDOG_S2);
    const int type = eldenPonRead(pon, ELDEN_PON_WATCHDOG_TYPE);
    const int control = eldenPonRead(pon, ELDEN_PON_WATCHDOG_ENABLE);
    IOLog("ELDEN_PON: arb=0x%x apid=%d reason=%d s1=%d s2=%d type=%d control=%d\n",
        version, channel, reason, s1, s2, type, control);
}
