// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#pragma push_macro("GetCurrentTime")
#include <unknwn.h>
#pragma pop_macro("GetCurrentTime")

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>

#include <cstdint>
#include <vector>

namespace ao::winui
{
  std::vector<winrt::Microsoft::UI::Xaml::Data::ItemIndexRange> trackSelectionRanges(
    std::vector<std::uint32_t> displayIndices);

  /// Replace the mounted source's selection without fetching a row for each selected index.
  void restoreTrackSelection(winrt::Microsoft::UI::Xaml::Controls::ListView const& rows,
                             std::vector<std::uint32_t> displayIndices);
} // namespace ao::winui
