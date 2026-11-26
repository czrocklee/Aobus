// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#pragma push_macro("GetCurrentTime")
#include <unknwn.h>
#pragma pop_macro("GetCurrentTime")

#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace ao::winui::layout
{
  /// An indexed track list whose UI Automation Selection pattern enumerates its selected rows.
  winrt::Microsoft::UI::Xaml::Controls::ListView makeTrackListView();
} // namespace ao::winui::layout
