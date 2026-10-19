// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace ao::audio
{
  struct PlaybackSegment final
  {
    std::uint64_t startFrame = 0;
    std::uint64_t frameCount = 0;
    std::uint64_t totalFrames = 0;

    bool operator==(PlaybackSegment const&) const = default;
  };

  // Audio-only playback input. Library/runtime metadata stays above lib/audio.
  struct PlaybackInput final
  {
    std::filesystem::path filePath{};
    std::chrono::milliseconds duration{0};
    std::uint32_t sampleRateHint = 0;
    std::uint8_t channelsHint = 0;
    std::uint8_t bitDepthHint = 0;
    // A segment requires a positive stored sampleRateHint.
    std::optional<PlaybackSegment> optSegment{};

    bool operator==(PlaybackInput const&) const = default;
  };
} // namespace ao::audio
