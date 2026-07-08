// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Transport.h>

#include <chrono>
#include <string>

namespace ao::tui
{
  std::string formatDuration(std::chrono::milliseconds duration);
  std::string transportLabel(audio::Transport transport);
  bool needsTransportClockTick(audio::Transport transport);
} // namespace ao::tui
