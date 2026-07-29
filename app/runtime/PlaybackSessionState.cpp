// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/PlaybackSessionState.h"

#include <algorithm>
#include <cmath>

namespace ao::rt
{
  float normalizePlaybackVolume(float volume) noexcept
  {
    if (!std::isfinite(volume))
    {
      return 1.0F;
    }

    return std::clamp(volume, 0.0F, 1.0F);
  }
} // namespace ao::rt
