// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackDetailSizing.h"

#include <algorithm>
#include <cstdint>

namespace ao::gtk::layout
{
  std::int32_t coverArtSideForWidth(std::int32_t const width, std::int32_t const targetSize)
  {
    if (targetSize <= 0)
    {
      return 0;
    }

    if (width <= 0)
    {
      return targetSize;
    }

    return std::clamp(width, 1, targetSize);
  }
} // namespace ao::gtk::layout
