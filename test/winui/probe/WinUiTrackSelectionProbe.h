// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <optional>
#include <string>
#include <vector>

namespace winrt::Microsoft::UI::Xaml::Controls
{
  struct ListView;
}

namespace ao::winui::test
{
  std::optional<std::string> trackSelectionProviderFailure(winrt::Microsoft::UI::Xaml::Controls::ListView const& rows,
                                                           std::vector<TrackId> const& expected);
} // namespace ao::winui::test
