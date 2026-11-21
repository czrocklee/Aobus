// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/SurfaceBrushResolver.h"
#include <ao/winui/Theme.h>

#include <optional>

namespace winrt::Microsoft::UI::Xaml::Controls
{
  struct Grid;
}

namespace ao::winui
{
  /// Apply the override palette and Classic corner radius to the frame root.
  void applyFrameTheme(winrt::Microsoft::UI::Xaml::Controls::Grid const& root, Theme const& theme);

  /// Clear the frame palette and restore the system background and Classic rounding.
  void applySystemFrameTheme(winrt::Microsoft::UI::Xaml::Controls::Grid const& root);

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
