// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include "core/playback/PlaybackTypes.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace app::core::decoder
{

  /**
   * @brief A block of decoded PCM data.
   */
  struct PcmBlock final
  {
    std::vector<std::byte> bytes;
    std::uint8_t bitDepth = 16;
    std::uint32_t frames = 0;
    std::uint64_t firstFrameIndex = 0;
    bool endOfStream = false;
  };

  /**
   * @brief Detailed information about the decoded stream.
   */
  struct DecodedStreamInfo final
  {
    AudioFormat sourceFormat;
    AudioFormat outputFormat;
    std::uint32_t durationMs = 0;
    bool isLossy = false;
  };

} // namespace app::core::decoder