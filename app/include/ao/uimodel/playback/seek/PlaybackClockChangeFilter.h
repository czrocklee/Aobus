// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/audio/Transport.h>
#include <ao/rt/playback/PlaybackSnapshot.h>

#include <chrono>

namespace ao::uimodel::detail
{
  /** Tracks the snapshot fields that establish a UI playback-clock anchor. */
  class PlaybackClockChangeFilter final
  {
  public:
    explicit PlaybackClockChangeFilter(rt::PlaybackTransportSnapshot const& transport) noexcept
      : _transport{transport.transport}, _positionRevision{transport.positionRevision}, _duration{transport.duration}
    {
    }

    bool update(rt::PlaybackTransportSnapshot const& transport) noexcept;

  private:
    audio::Transport _transport = audio::Transport::Idle;
    rt::PlaybackPositionRevision _positionRevision{};
    std::chrono::milliseconds _duration{0};
  };
} // namespace ao::uimodel::detail
