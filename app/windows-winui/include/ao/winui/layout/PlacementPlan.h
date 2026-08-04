// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <optional>

namespace ao::uimodel
{
  struct LayoutNode;
}

namespace ao::winui
{
  /// How a parent container sizes the slot it allocates to a child on one axis.
  enum class SlotSizing : std::uint8_t
  {
    Auto,
    Star,
  };

  enum class HorizontalAlignment : std::uint8_t
  {
    Left,
    Center,
    Right,
    Stretch,
  };

  enum class VerticalAlignment : std::uint8_t
  {
    Top,
    Center,
    Bottom,
    Stretch,
  };

  /**
   * @brief WinUI interpretation of one node's version 1 common layout fields.
   *
   * The plan is deliberately split by who applies it. The parent container
   * applies @ref horizontalSlot and @ref verticalSlot when it allocates the
   * child's row or column, because WinUI expresses "take the remaining space"
   * on the containing definition rather than on the child. Everything else is a
   * local value on the child element, which is what makes an authored field win
   * over a `Style` setter.
   *
   * An unset optional means the document authored nothing, so a style default
   * stays in effect.
   */
  struct PlacementPlan final
  {
    SlotSizing horizontalSlot = SlotSizing::Auto;
    SlotSizing verticalSlot = SlotSizing::Auto;
    std::optional<HorizontalAlignment> optHorizontalAlignment{};
    std::optional<VerticalAlignment> optVerticalAlignment{};
    std::optional<double> optMinWidth{};
    std::optional<double> optMinHeight{};
    bool authoredVisible = true;

    friend bool operator==(PlacementPlan const&, PlacementPlan const&) = default;
  };

  /**
   * @brief Placement for @p node, assuming its layout fields already passed catalog validation.
   *
   * Malformed values cannot reach a constructed element: validation rejects the
   * whole candidate first, so this mapping ignores anything it cannot interpret.
   */
  PlacementPlan planPlacement(uimodel::LayoutNode const& node);
} // namespace ao::winui
