// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

// PMK8550 on SPMI bus 0, SID 0. The alternate Linux DTS stores a UTC offset
// in SDAM2 at 0xb0. That cell is optional on Elden's other Linux builds.
#define ELDEN_RTC_PPID 0x061U
#define ELDEN_RTC_CTRL 0x46U
#define ELDEN_RTC_COUNTER 0x48U
#define ELDEN_RTC_SDAM_PPID 0x071U
#define ELDEN_RTC_SDAM_OFFSET 0xb0U
