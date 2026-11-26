// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "track/TrackListSelection.h"

#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ao::winui
{
  using winrt::Microsoft::UI::Xaml::Data::ItemIndexRange;

  std::vector<ItemIndexRange> trackSelectionRanges(std::vector<std::uint32_t> displayIndices)
  {
    auto ranges = std::vector<ItemIndexRange>{};
    std::ranges::sort(displayIndices);
    auto const duplicates = std::ranges::unique(displayIndices);
    displayIndices.erase(duplicates.begin(), duplicates.end());

    for (std::size_t start = 0; start < displayIndices.size();)
    {
      auto end = start + 1;

      while (end < displayIndices.size() && displayIndices[end] - displayIndices[end - 1] == 1)
      {
        ++end;
      }

      ranges.emplace_back(static_cast<std::int32_t>(displayIndices[start]), static_cast<std::uint32_t>(end - start));
      start = end;
    }

    return ranges;
  }

  void restoreTrackSelection(winrt::Microsoft::UI::Xaml::Controls::ListView const& rows,
                             std::vector<std::uint32_t> displayIndices)
  {
    for (auto const& range : rows.SelectedRanges())
    {
      rows.DeselectRange(range);
    }

    for (auto const& range : trackSelectionRanges(std::move(displayIndices)))
    {
      rows.SelectRange(range);
    }
  }
} // namespace ao::winui
