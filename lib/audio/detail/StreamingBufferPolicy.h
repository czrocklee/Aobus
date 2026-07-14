// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ao::audio::detail
{
  inline std::size_t bufferByteCountForDuration(std::uint64_t bytesPerSecond,
                                                std::chrono::milliseconds duration,
                                                std::size_t capacity) noexcept
  {
    if (bytesPerSecond == 0 || duration <= std::chrono::milliseconds{0} || capacity == 0)
    {
      return 0;
    }

    auto const milliseconds = static_cast<std::uint64_t>(duration.count());
    auto const maximum = std::numeric_limits<std::uint64_t>::max();

    if (milliseconds > maximum / bytesPerSecond)
    {
      return capacity;
    }

    auto const scaledBytes = milliseconds * bytesPerSecond;
    auto const wholeBytes = scaledBytes / 1000U;
    auto const roundedBytes = wholeBytes + static_cast<std::uint64_t>((scaledBytes % 1000U) != 0U);
    return static_cast<std::size_t>(std::min(roundedBytes, static_cast<std::uint64_t>(capacity)));
  }

  inline bool permitsDecode(std::size_t targetByteCount,
                            std::size_t bufferedByteCount,
                            std::size_t writableByteCount,
                            std::size_t previousBlockByteCount) noexcept
  {
    if (bufferedByteCount >= targetByteCount)
    {
      return false;
    }

    return previousBlockByteCount == 0 || writableByteCount >= previousBlockByteCount;
  }
} // namespace ao::audio::detail
