// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <cstdint>

namespace rs::audio
{
  /**
   * @brief Describes the physical format of a PCM audio stream.
   */
  struct Format final
  {
    std::uint32_t sampleRate = 0;
    std::uint8_t channels = 0;
    std::uint8_t bitDepth = 0;
    std::uint8_t validBits = 0;
    bool isFloat = false;
    bool isInterleaved = true;

    bool operator==(Format const&) const = default;
  };
} // namespace rs::audio