// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>

#include <cstdint>
#include <functional>
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

  enum class PlaybackFailureDisposition : std::uint8_t
  {
    Unhandled,
    Recovered,
    Stopped,
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
    PlaybackFailureDisposition disposition = PlaybackFailureDisposition::Unhandled;
  };

  using PlaybackFailureRecoveryHandler = std::move_only_function<PlaybackFailureDisposition(PlaybackFailure const&)>;
} // namespace ao::rt
