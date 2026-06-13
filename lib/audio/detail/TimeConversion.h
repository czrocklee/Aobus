// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace ao::audio::detail
{
  inline std::uint64_t saturatingScale(std::uint64_t value, std::uint32_t multiplier, std::uint32_t divisor) noexcept
  {
    if (divisor == 0)
    {
      return 0;
    }

    auto const wholeUnits = value / divisor;
    auto const remainder = value % divisor;
    auto const maximum = std::numeric_limits<std::uint64_t>::max();

    if (multiplier != 0 && wholeUnits > maximum / multiplier)
    {
      return maximum;
    }

    auto const whole = wholeUnits * multiplier;
    auto const fraction = (remainder * multiplier) / divisor;

    if (fraction > maximum - whole)
    {
      return maximum;
    }

    return whole + fraction;
  }

  inline std::chrono::milliseconds convertToDuration(std::uint64_t duration, std::uint32_t timescale) noexcept
  {
    auto const milliseconds = saturatingScale(duration, 1000, timescale);
    return std::chrono::milliseconds{
      static_cast<std::uint32_t>(std::min<std::uint64_t>(milliseconds, std::numeric_limits<std::uint32_t>::max()))};
  }
} // namespace ao::audio::detail
