// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::rt
{
  enum class ShuffleMode : std::uint8_t
  {
    Off,
    On,
  };

  enum class RepeatMode : std::uint8_t
  {
    Off,
    One,
    All,
  };
} // namespace ao::rt
