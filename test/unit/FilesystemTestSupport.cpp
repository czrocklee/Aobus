// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "FilesystemTestSupport.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ao::test
{
  std::string formatFileTime(std::filesystem::file_time_type const fileTime)
  {
    auto ticks = fileTime.time_since_epoch().count();
    auto digits = std::string{};

    if (ticks == 0)
    {
      digits = "0";
    }

    // The remainder carries the sign of the dividend, so the magnitude is
    // never negated and the most negative representable count still prints.
    auto const negative = ticks < 0;

    while (ticks != 0)
    {
      auto const digit = static_cast<std::int32_t>(ticks % 10);
      ticks /= 10;
      digits.push_back(static_cast<char>('0' + (digit < 0 ? -digit : digit)));
    }

    if (negative)
    {
      digits.push_back('-');
    }

    std::ranges::reverse(digits);
    return digits + "ns since the file clock epoch";
  }
} // namespace ao::test
