// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "SelectionNavigation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <string>

namespace ao::tui
{
  std::string selectionSummary(std::size_t const trackCount, std::int32_t const selectedIndex)
  {
    if (trackCount == 0)
    {
      return "0 tracks";
    }

    auto const visibleIndex = clampSelection(static_cast<std::size_t>(std::max(0, selectedIndex)), trackCount) + 1;
    return std::format("{} / {} tracks", visibleIndex, trackCount);
  }

  std::int32_t moveSelection(std::int32_t const selectedIndex, std::int32_t const delta, std::size_t const itemCount)
  {
    if (itemCount == 0)
    {
      return 0;
    }

    auto const maxIndex =
      std::min<std::int64_t>(static_cast<std::int64_t>(itemCount - 1), std::numeric_limits<std::int32_t>::max());
    auto const next = static_cast<std::int64_t>(selectedIndex) + static_cast<std::int64_t>(delta);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(next, 0, maxIndex));
  }

  std::size_t clampSelection(std::size_t const selection, std::size_t const itemCount)
  {
    if (itemCount == 0)
    {
      return 0;
    }

    return std::min(selection, itemCount - 1);
  }
} // namespace ao::tui
