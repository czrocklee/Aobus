// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/winui/layout/ThemeSurface.h>

#include <winrt/Microsoft.UI.Xaml.Media.h>

#include <functional>

namespace ao::winui::layout
{
  /**
   * @brief Brush the frame paints a themed surface with.
   *
   * The shell owns both halves a component must not know: whether a theme
   * override is loaded at all, and what the stock brush for a slot is when it
   * is not. A null brush means the element keeps whatever background it built
   * itself with.
   */
  using SurfaceBrushResolver = std::function<winrt::Microsoft::UI::Xaml::Media::Brush(ThemeSurface)>;
} // namespace ao::winui::layout
