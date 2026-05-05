// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/audio/Types.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::audio
{
  struct PcmBlock final
  {
    std::span<std::byte const> bytes;
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
    Format sourceFormat;
    Format outputFormat;
    std::uint32_t durationMs = 0;
    bool isLossy = false;
  };
} // namespace ao::audio