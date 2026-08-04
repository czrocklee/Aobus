// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

#include <string_view>

namespace ao::winui::layout
{
  /// Find a frame-owned resource, preferring the window scope over the application fallback.
  winrt::Windows::Foundation::IInspectable lookupResource(
    winrt::Microsoft::UI::Xaml::ResourceDictionary const& resources,
    std::string_view key);
} // namespace ao::winui::layout
