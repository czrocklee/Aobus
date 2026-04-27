// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <cstdint>

namespace app::core
{

  /**
   * @brief Describes the physical format of a PCM audio stream.
   */
  struct AudioFormat final
  {
    std::uint32_t sampleRate = 0;
    std::uint8_t channels = 0;
    std::uint8_t bitDepth = 0;
    bool isFloat = false;
    bool isInterleaved = true;

    bool operator==(AudioFormat const&) const = default;
  };

} // namespace app::core