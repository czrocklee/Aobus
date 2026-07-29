// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/playback/seek/PlaybackClockChangeFilter.h>

#include <ao/rt/playback/PlaybackSnapshot.h>

namespace ao::uimodel::detail
{
  bool PlaybackClockChangeFilter::update(rt::PlaybackTransportSnapshot const& transport) noexcept
  {
    if (transport.transport == _transport && transport.positionRevision == _positionRevision &&
        transport.duration == _duration)
    {
      return false;
    }

    _transport = transport.transport;
    _positionRevision = transport.positionRevision;
    _duration = transport.duration;
    return true;
  }
} // namespace ao::uimodel::detail
