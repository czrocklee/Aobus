// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::audio::detail
{
  struct AacStreamConfig final
  {
    std::uint32_t sampleRate = 0;
    std::uint8_t channels = 0;
  };

  AacStreamConfig parseAudioSpecificConfig(std::span<std::byte const> bytes) noexcept;
} // namespace ao::audio::detail
