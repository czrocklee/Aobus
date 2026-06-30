// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::audio
{
  enum class Transport : std::uint8_t
  {
    Idle,
    Opening,
    Buffering,
    Playing,
    Paused,
    Seeking,
    Stopping,
    Error,
  };
} // namespace ao::audio
