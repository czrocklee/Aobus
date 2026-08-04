// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/CommonLayoutProps.h"

#include <winrt/Microsoft.UI.Xaml.Media.h>

#include <optional>
#include <string_view>

namespace ao::winui
{
  struct Theme;

  /// Brush for an `#RRGGBB` or `#AARRGGBB` theme token.
  winrt::Microsoft::UI::Xaml::Media::SolidColorBrush themeColorBrush(std::string_view token);

  /**
   * @brief Resolver a generation paints its authored `surface` slots with.
   *
   * With an override loaded, every slot resolves to that theme's own token.
   * Without one the stock brushes apply, and the slots the system theme leaves
   * untouched resolve to nothing, so those elements keep whatever background
   * they were built with rather than being flattened to a chosen colour.
   */
  layout::SurfaceBrushResolver makeSurfaceBrushResolver(std::optional<Theme> optTheme);
} // namespace ao::winui
