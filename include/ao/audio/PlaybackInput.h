// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>

namespace ao::audio
{
  // Audio-only playback input. Library/runtime metadata stays above lib/audio.
  struct PlaybackInput final
  {
    std::filesystem::path filePath{};
    std::chrono::milliseconds duration{0};
    std::uint32_t sampleRateHint = 0;
    std::uint8_t channelsHint = 0;
    std::uint8_t bitDepthHint = 0;

    bool operator==(PlaybackInput const&) const = default;
  };
} // namespace ao::audio
