// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInteraction.h>

#include <chrono>
#include <variant>

namespace ao::appkit
{
  // A gesture belongs to the displayed timeline, including replay of the same track.
  struct PlaybackSeekTarget final
  {
    rt::PlaybackOccurrenceId occurrenceId{};
    rt::PlaybackPositionRevision revision{};
    std::chrono::milliseconds duration{0};
  };

  struct AbsoluteSeek final
  {
    PlaybackSeekTarget target{};
    uimodel::SeekSliderUpdate update{};
  };

  struct RelativeSeek final
  {
    rt::PlaybackOccurrenceId occurrenceId{};
    std::chrono::milliseconds delta{0};
  };

  using PlaybackSeekIntent = std::variant<std::monostate, AbsoluteSeek, RelativeSeek>;
} // namespace ao::appkit
