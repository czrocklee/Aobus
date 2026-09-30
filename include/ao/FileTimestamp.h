// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <compare>
#include <cstdint>

namespace ao
{
  /**
   * A file modification instant on the Unix/POSIX time scale. Seconds are
   * measured from 1970-01-01 00:00:00 UTC, excluding leap seconds. Nanoseconds
   * are the nonnegative fraction of that second, in [0, 1000000000).
   * Absence is represented separately with std::optional<FileTimestamp>.
   */
  struct FileTimestamp final
  {
    static constexpr std::uint32_t kNanosecondsPerSecond = 1'000'000'000;

    std::int64_t seconds = 0;
    std::uint32_t nanoseconds = 0;

    constexpr bool isNormalized() const noexcept { return nanoseconds < kNanosecondsPerSecond; }

    std::strong_ordering operator<=>(FileTimestamp const&) const = default;
  };
} // namespace ao
