// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::audio
{
  enum class SampleKind : std::uint8_t
  {
    Integer,
    FloatingPoint,
  };

  /** @brief Logical audio signal properties, independent of PCM storage. */
  struct SignalFormat final
  {
    std::uint32_t sampleRate = 0;
    std::uint8_t channels = 0;
    std::uint8_t precisionBits = 0;
    SampleKind sampleKind = SampleKind::Integer;

    bool operator==(SignalFormat const&) const = default;
  };
} // namespace ao::audio
