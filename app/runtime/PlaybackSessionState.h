// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/PlaybackState.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ao::rt
{
  inline constexpr auto kPlaybackSessionConfigGroup = std::string_view{"playback-session"};
  inline constexpr std::uint32_t kPlaybackSessionSchemaVersion = 2;

  struct PlaybackSessionState final
  {
    std::uint32_t schemaVersion = kPlaybackSessionSchemaVersion;
    std::vector<TrackId> queueTrackIds{};
    std::uint64_t currentQueueIndex = 0;
    ListId sourceListId = kInvalidListId;
    TrackId trackId = kInvalidTrackId;
    std::uint64_t positionMs = 0;
    ShuffleMode shuffleMode = ShuffleMode::Off;
    RepeatMode repeatMode = RepeatMode::Off;
    float volume = 1.0F;
    bool muted = false;
  };

  inline float normalizePlaybackVolume(float volume) noexcept
  {
    if (!std::isfinite(volume))
    {
      return 1.0F;
    }

    return std::clamp(volume, 0.0F, 1.0F);
  }

  inline ShuffleMode normalizeShuffleMode(ShuffleMode mode) noexcept
  {
    switch (mode)
    {
      case ShuffleMode::Off:
      case ShuffleMode::On: return mode;
    }

    return ShuffleMode::Off;
  }

  inline RepeatMode normalizeRepeatMode(RepeatMode mode) noexcept
  {
    switch (mode)
    {
      case RepeatMode::Off:
      case RepeatMode::One:
      case RepeatMode::All: return mode;
    }

    return RepeatMode::Off;
  }

  inline PlaybackSessionState normalizePlaybackSessionState(PlaybackSessionState session) noexcept
  {
    auto const maxPositionMs = static_cast<std::uint64_t>(std::chrono::milliseconds::max().count());

    session.positionMs = std::min(session.positionMs, maxPositionMs);
    session.shuffleMode = normalizeShuffleMode(session.shuffleMode);
    session.repeatMode = normalizeRepeatMode(session.repeatMode);
    session.volume = normalizePlaybackVolume(session.volume);
    return session;
  }
} // namespace ao::rt
