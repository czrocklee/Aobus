// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/winui/layout/PlacementPlan.h>

#include <winrt/Microsoft.UI.Xaml.h>

#include <memory>
#include <vector>

namespace ao::winui::layout
{
  /**
   * @brief One constructed Windows element together with everything its node owns.
   *
   * A component belongs to exactly one view generation. When that generation is
   * retired the component is destroyed, taking its element, event revokers, and
   * view-local subscriptions with it.
   */
  class LayoutComponent
  {
  public:
    LayoutComponent() = default;
    LayoutComponent(LayoutComponent const&) = delete;
    LayoutComponent& operator=(LayoutComponent const&) = delete;
    LayoutComponent(LayoutComponent&&) = delete;
    LayoutComponent& operator=(LayoutComponent&&) = delete;
    virtual ~LayoutComponent() = default;

    /// The element this component contributes to its parent's child region.
    virtual winrt::Microsoft::UI::Xaml::FrameworkElement element() const = 0;
  };

  /// A built component and the placement its parent must allocate for it.
  struct PlacedChild final
  {
    std::unique_ptr<LayoutComponent> componentPtr;
    PlacementPlan placement;
  };

  /**
   * @brief A component that owns a child region.
   *
   * Children arrive already built so the container can decide native placement
   * in one pass: WinUI expresses remaining-space allocation on the row or column
   * definition, which only the parent can create.
   */
  class LayoutContainer : public LayoutComponent
  {
  public:
    virtual void adopt(std::vector<PlacedChild> children) = 0;
  };
} // namespace ao::winui::layout
