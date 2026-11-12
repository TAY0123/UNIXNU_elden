// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "EldenPmicRtc.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <initializer_list>

struct Bus {
    uint32_t version = 0x70000000, count = 4;
    std::array<unsigned, 4> ppid = {0x061, 0x061, 0x071, 0x010};
    std::array<unsigned, 4> ee = {1, 0, 0, 0};

    uint32_t core(unsigned offset) const
    {
        if (offset == 0) return version;
        if (offset == 4) return count;
        return ppid.at((offset - 0x2000) / 4) << 8;
    }
    uint32_t owner(unsigned offset) const { return ee.at(offset / 4); }
};

struct Transaction {
    unsigned channel, reg, bytes;
    uint32_t data, status = 1;
};

struct Observer {
    std::initializer_list<Transaction> expected;
    unsigned issued = 0, delays = 0;
    bool pending = false;

    void command(unsigned address, uint32_t value)
    {
        assert(issued < expected.size());
        const auto &step = expected.begin()[issued++];
        assert(address == step.channel * elden_rtc::ObserverStride);
        assert(value == ((1U << 27) | (step.reg << 4) | (step.bytes - 1)));
        pending = true;
    }

    uint32_t load(unsigned address)
    {
        const auto &step = expected.begin()[issued - 1];
        unsigned reg = address - step.channel * elden_rtc::ObserverStride;
        if (reg == 8) return pending ? 0 : step.status;
        assert(reg == 0x18 && !pending && step.status == 1);
        return step.data;
    }

    void delay() { pending = false; ++delays; }
};

int main()
{
    Bus bus;
    assert(elden_rtc::observerChannel(bus, ELDEN_RTC_PPID) == 1);
    assert(elden_rtc::observerChannel(bus, ELDEN_RTC_SDAM_PPID) == 2);
    assert(elden_rtc::observerChannel(bus, 0x555) == -1);
    bus.ee[0] = 0;
    assert(elden_rtc::observerChannel(bus, ELDEN_RTC_PPID) == -1);
    bus.version = 0x80000000;
    assert(elden_rtc::observerChannel(bus, ELDEN_RTC_SDAM_PPID) == -1);
    bus.version = 0x70000000;
    bus.count = 1025;
    assert(elden_rtc::observerChannel(bus, ELDEN_RTC_SDAM_PPID) == -1);

    constexpr uint32_t epoch = 1700000000;
    uint32_t value = 0;
    Observer raw{{ {1, 0x46, 1, 0x80}, {1, 0x48, 4, epoch}, {1, 0x48, 1, epoch + 1} }};
    assert(elden_rtc::utc(raw, 1, -1, value) && value == epoch && raw.issued == 3);
    Observer carry{{ {1, 0x46, 1, 0x80}, {2, 0xb0, 4, epoch},
        {1, 0x48, 4, 255}, {1, 0x48, 1, 0}, {1, 0x48, 4, 256}, {1, 0x48, 1, 1} }};
    assert(elden_rtc::utc(carry, 1, 2, value) && value == epoch + 256 && carry.issued == 6);
    Observer unset{{ {1, 0x46, 1, 0x80}, {1, 0x48, 4, 42}, {1, 0x48, 1, 43} }};
    assert(!elden_rtc::utc(unset, 1, -1, value) && value == epoch + 256);
    Observer disabled{{ {1, 0x46, 1, 0} }};
    assert(!elden_rtc::utc(disabled, 1, -1, value));
    Observer denied{{ {1, 0x46, 1, 0x80}, {2, 0xb0, 4, 0, 5} }};
    assert(!elden_rtc::utc(denied, 1, 2, value) && denied.issued == 2);
    Observer timeout{{ {1, 0x46, 1, 0, 0} }};
    assert(!elden_rtc::utc(timeout, 1, -1, value) && timeout.delays == 1000);
    assert(!elden_rtc::read(timeout, 1024, 0x48, 4, value));
    assert(!elden_rtc::read(timeout, 1, 0xff, 4, value));
    puts("PASS: SPMI observer mapping, raw UTC, SDAM offset, rollover and failure bounds");
}
