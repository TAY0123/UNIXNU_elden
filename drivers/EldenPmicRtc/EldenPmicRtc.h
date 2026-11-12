// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <stdint.h>
#include "EldenRtcResources.h"

namespace elden_rtc {

constexpr unsigned ObserverStride = 0x20;
constexpr uint32_t EarliestUtc = 1577836800; // 2020-01-01

template <class Bus>
int observerChannel(Bus &bus, unsigned ppid)
{
    const uint32_t version = bus.core(0);
    const unsigned count = bus.core(4) & 0x7ff;
    if ((version >> 28) != 7 || !count || count > 1024) return -1;
    int channel = -1;
    for (unsigned apid = 0; apid < count; ++apid) {
        if (((bus.core(0x2000 + apid * 4) >> 8) & 0xfff) != ppid || (bus.owner(apid * 4) & 7)) continue;
        if (channel >= 0) return -1;
        channel = static_cast<int>(apid);
    }
    return channel;
}

template <class Observer>
bool read(Observer &observer, unsigned channel, unsigned reg, unsigned bytes, uint32_t &value)
{
    if (channel >= 1024 || !bytes || bytes > 4 || reg > 256 - bytes) return false;
    constexpr uint32_t ExtendedRead = 1U << 27, Done = 1, Errors = 0x0e;
    const unsigned offset = channel * ObserverStride;
    observer.command(offset, ExtendedRead | (reg << 4) | (bytes - 1));
    for (unsigned i = 0; i < 1000; ++i) {
        const uint32_t status = observer.load(offset + 8);
        if (status & Done) {
            if (status & Errors) return false;
            const uint32_t raw = observer.load(offset + 0x18);
            value = bytes == 4 ? raw : raw & ((1U << (bytes * 8)) - 1U);
            return true;
        }
        observer.delay();
    }
    return false;
}

template <class Observer>
bool utc(Observer &observer, unsigned rtc, int sdam, uint32_t &seconds)
{
    uint32_t control;
    if (!read(observer, rtc, ELDEN_RTC_CTRL, 1, control) || !(control & 0x80)) return false;
    uint32_t offset = 0;
    if (sdam >= 0 && !read(observer, static_cast<unsigned>(sdam), ELDEN_RTC_SDAM_OFFSET, 4, offset)) return false;

    // Check the low byte after the counter read so a carry cannot leave a torn value.
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t counter, low;
        if (!read(observer, rtc, ELDEN_RTC_COUNTER, 4, counter) ||
            !read(observer, rtc, ELDEN_RTC_COUNTER, 1, low)) return false;
        if (low < (counter & 0xff)) continue;
        const uint32_t utc = counter + offset;
        if (utc < EarliestUtc) return false;
        seconds = utc;
        return true;
    }
    return false;
}

} // namespace elden_rtc
