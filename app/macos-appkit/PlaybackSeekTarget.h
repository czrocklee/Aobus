// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/rt/playback/PlaybackSnapshot.h>

#include <chrono>

namespace ao::appkit
{
  // A gesture belongs to the displayed timeline, including replay of the same track.
  struct PlaybackSeekTarget final
  {
    rt::PlaybackPositionRevision revision{};
    std::chrono::milliseconds duration{0};
  };
} // namespace ao::appkit
