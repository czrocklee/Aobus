// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::audio
{
  enum class Quality : std::uint8_t
  {
    Unknown,
    BitwisePerfect,
    LosslessPadded,
    LosslessFloat,
    LinearIntervention,
    LossySource,
    Clipped,
  };
} // namespace ao::audio
