// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::uimodel
{
  enum class PlaybackCommand : std::uint8_t
  {
    Play,
    Pause,
    PlayPause,
    Stop,
    Next,
    Previous,
    ToggleShuffle,
    CycleRepeat,
  };
} // namespace ao::uimodel
