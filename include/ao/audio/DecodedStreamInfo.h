// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/audio/Format.h>

#include <chrono>

namespace ao::audio
{
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
