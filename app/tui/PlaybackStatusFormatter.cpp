// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "PlaybackStatusFormatter.h"

#include <ao/audio/Transport.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>

#include <chrono>
#include <string>
#include <utility>

namespace ao::tui
{
  std::string formatDuration(std::chrono::milliseconds const duration)
  {
    auto formatted = uimodel::formatDuration(duration);
    return formatted.empty() ? std::string{"0:00"} : std::move(formatted);
  }

  std::string transportLabel(audio::Transport const transport)
  {
    switch (transport)
    {
      case audio::Transport::Idle: return "Idle";
      case audio::Transport::Opening: return "Opening";
      case audio::Transport::Buffering: return "Buffering";
      case audio::Transport::Playing: return "Playing";
      case audio::Transport::Paused: return "Paused";
      case audio::Transport::Seeking: return "Seeking";
      case audio::Transport::Stopping: return "Stopping";
      case audio::Transport::Error: return "Error";
    }

    return "Unknown";
  }

  bool shouldTickTransportClock(audio::Transport const transport)
  {
    switch (transport)
    {
      case audio::Transport::Opening:
      case audio::Transport::Buffering:
      case audio::Transport::Playing:
      case audio::Transport::Seeking: return true;
      case audio::Transport::Idle:
      case audio::Transport::Paused:
      case audio::Transport::Stopping:
      case audio::Transport::Error: return false;
    }

    return false;
  }
} // namespace ao::tui
