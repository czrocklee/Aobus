// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/TrackField.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  inline constexpr auto kPlaybackSessionConfigGroup = std::string_view{"playback-session"};
  inline constexpr std::uint32_t kPlaybackSessionSchemaVersion = 3;
  inline constexpr std::size_t kPlaybackSessionMaxSortTerms = kTrackSortFieldCount;

  struct PlaybackSessionState final
  {
    std::uint32_t schemaVersion = kPlaybackSessionSchemaVersion;
    ListId sourceListId = kInvalidListId;
    std::string quickFilterExpression{};
    std::vector<TrackSortTerm> sortBy{};
    TrackId currentTrackId = kInvalidTrackId;
    std::uint64_t anchorIndex = 0;
    std::uint64_t positionMs = 0;
    ShuffleMode shuffleMode = ShuffleMode::Off;
    RepeatMode repeatMode = RepeatMode::Off;
    float volume = 1.0F;
    bool muted = false;

    bool operator==(PlaybackSessionState const&) const = default;
  };

  /** Internal transport half of a playback-session snapshot. */
  struct PlaybackTransportSessionState final
  {
    ListId sourceListId = kInvalidListId;
    TrackId trackId = kInvalidTrackId;
    std::uint64_t positionMs = 0;
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
} // namespace ao::rt
