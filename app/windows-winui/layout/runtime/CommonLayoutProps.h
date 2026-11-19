// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/SurfaceBrushResolver.h"
#include <ao/Error.h>
#include <ao/winui/layout/ElementKind.h>
#include <ao/winui/layout/PlacementPlan.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <span>

namespace ao::uimodel
{
  struct LayoutNode;
}

namespace ao::winui::layout
{
  /**
   * @brief Apply the child-local half of @p plan, any authored `styleKey`, and any authored `surface` to @p element.
   *
   * The style is resolved first and only supplies defaults; every authored
   * placement field is then written as a local value, which always wins over a
   * style setter. A `styleKey` that does not resolve in @p resources, or whose
   * `TargetType` does not accept @p kind, fails and therefore rejects the whole
   * candidate. The themed surface is applied last, because colour is the one
   * thing a style cannot carry across a theme change.
   */
  Result<> applyCommonProps(winrt::Microsoft::UI::Xaml::FrameworkElement const& element,
                            uimodel::LayoutNode const& node,
                            PlacementPlan const& plan,
                            ElementKind kind,
                            winrt::Microsoft::UI::Xaml::ResourceDictionary const& resources,
                            SurfaceBrushResolver const& surfaceBrush);

  /**
   * @brief Lay @p children out in @p grid as one row or column per child.
   *
   * Each child's slot is star-sized when it authored expansion on the container
   * axis and auto-sized otherwise, which is how WinUI expresses `hexpand` and
   * `vexpand`. On the cross axis the child's own alignment already applies.
   */
  void placeChildrenInGrid(winrt::Microsoft::UI::Xaml::Controls::Grid const& grid,
                           std::span<PlacedChild const> children,
                           bool vertical);
} // namespace ao::winui::layout
