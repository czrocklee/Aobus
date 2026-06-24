// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/audio/Format.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::audio
{
  struct PcmBlock final
  {
    /// @brief Aliasing span of decoder-owned memory; valid until next readNextBlock() or decoder close.
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
    std::chrono::milliseconds duration{0};
    bool isLossy = false;
    AudioCodec codec = AudioCodec::Unknown;
  };
} // namespace ao::audio
