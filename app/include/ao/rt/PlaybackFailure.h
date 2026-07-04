// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>

#include <cstdint>
#include <string>

namespace ao::rt
{
  enum class PlaybackFailureKind : std::uint8_t
  {
    TrackOpen,
    Decode,
    RouteActivation,
    DeviceLost,
  };

  struct PlaybackFailure final
  {
    PlaybackFailureKind kind = PlaybackFailureKind::TrackOpen;
    TrackId trackId = kInvalidTrackId;
    ListId sourceListId = kInvalidListId;
    std::uint64_t generation = 0;
    Error error;
    bool recoverable = false;
    std::string title{};
  };
} // namespace ao::rt
