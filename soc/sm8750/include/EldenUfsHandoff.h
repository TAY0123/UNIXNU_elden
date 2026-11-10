// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <stddef.h>
#include <stdint.h>

// Offsets in the SM8750 GCC UFS page.
enum : uint32_t {
  kEldenUfsGdsc = 0x04,
  kEldenUfsAxi = 0x18,
  kEldenUfsAhb = 0x28,
  kEldenUfsAggregate = 0xf0,
};

static constexpr bool eldenUfsClockOn (uint32_t status)
{
  return (status & 3) && (!(status & 0x80000000U) || (status & 2));
}

static constexpr bool eldenUfsClocksReady (uint32_t gdsc, uint32_t axi, uint32_t ahb,
                                          uint32_t aggregate)
{
  return (gdsc & 0x80000000U) && eldenUfsClockOn (axi) && eldenUfsClockOn (ahb) &&
         eldenUfsClockOn (aggregate);
}

static constexpr bool eldenUfsLegacyQueue (uint32_t version, uint32_t memConfig)
{
  const unsigned major = (version >> 8) & 0xff;
  return major >= 2 && major <= 4 && !(memConfig & 1U);
}

// Match a complete element of the ADT compatible string list.
static bool eldenUfsBoardMatches (const char *data, size_t size)
{
  static constexpr char board[] = "lenovo,elden";
  size_t start = 0;
  while (start < size) {
    size_t end = start;
    while (end < size && data[end]) ++end;
    if (end == size) return false;
    if (end - start == sizeof (board) - 1) {
      size_t i = 0;
      while (i < sizeof (board) - 1 && data[start + i] == board[i]) ++i;
      if (i == sizeof (board) - 1) return true;
    }
    start = end + 1;
  }
  return false;
}
