// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <chrono>
#include <cstdint>

namespace ao::audio
{
  constexpr std::uint64_t durationToSamples(std::chrono::milliseconds const duration,
                                            std::uint32_t const sampleRate) noexcept
  {
    if (duration.count() <= 0)
    {
      return 0;
    }

    return (static_cast<std::uint64_t>(duration.count()) * sampleRate) / 1000U;
  }

  constexpr std::chrono::milliseconds samplesToDuration(std::uint64_t const samples,
                                                        std::uint32_t const sampleRate) noexcept
  {
    if (sampleRate == 0U)
    {
      return std::chrono::milliseconds{0};
    }

    return std::chrono::milliseconds{(samples * 1000U) / sampleRate};
  }
} // namespace ao::audio
