// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/TrackField.h>
#include <ao/utility/Uuid.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  inline constexpr auto kPlaybackSessionConfigGroup = std::string_view{"playback-session"};
  inline constexpr std::uint32_t kPlaybackSessionSchemaVersion = 5;
  inline constexpr std::size_t kPlaybackSessionMaxSortTerms = kTrackSortFieldCount;

  struct PlaybackSessionState final
  {
    std::uint32_t schemaVersion = kPlaybackSessionSchemaVersion;
    // The library the session was saved for; its ids mean nothing in another.
    utility::UuidBytes libraryId{};
    ListId sourceListId = kInvalidListId;
    std::string quickFilterExpression{};
    std::vector<TrackSortTerm> sortBy{};
    TrackId currentTrackId = kInvalidTrackId;
    // Canonical library URI of the current track's file as the library last
    // confirmed it, which follows a relink while the app runs.
    std::string currentTrackUri{};
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
    std::string trackUri{};
    std::uint64_t positionMs = 0;
    float volume = 1.0F;
    bool muted = false;
  };

  float normalizePlaybackVolume(float volume) noexcept;
} // namespace ao::rt
