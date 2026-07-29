// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <chrono>
#include <cstdint>

namespace ao::audio::detail
{
  std::uint64_t saturatingScale(std::uint64_t value, std::uint32_t multiplier, std::uint32_t divisor) noexcept;
  std::chrono::milliseconds convertToDuration(std::uint64_t duration, std::uint32_t timescale) noexcept;
} // namespace ao::audio::detail
