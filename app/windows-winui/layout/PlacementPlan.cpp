// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/PlacementPlan.h>

#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPlacement.h>

#include <optional>

namespace ao::winui
{
  namespace
  {
    /// A slot takes the remaining space only when the document asked it to.
    SlotSizing slotSizing(std::optional<bool> const& optExpand)
    {
      return optExpand.value_or(false) ? SlotSizing::Star : SlotSizing::Auto;
    }

    HorizontalAlignment toHorizontal(uimodel::LayoutAlignment const alignment)
    {
      switch (alignment)
      {
        case uimodel::LayoutAlignment::Fill: return HorizontalAlignment::Stretch;
        case uimodel::LayoutAlignment::Start: return HorizontalAlignment::Left;
        case uimodel::LayoutAlignment::End: return HorizontalAlignment::Right;
        case uimodel::LayoutAlignment::Center: return HorizontalAlignment::Center;
      }

      return HorizontalAlignment::Stretch;
    }

    VerticalAlignment toVertical(uimodel::LayoutAlignment const alignment)
    {
      switch (alignment)
      {
        case uimodel::LayoutAlignment::Fill: return VerticalAlignment::Stretch;
        case uimodel::LayoutAlignment::Start: return VerticalAlignment::Top;
        case uimodel::LayoutAlignment::End: return VerticalAlignment::Bottom;
        case uimodel::LayoutAlignment::Center: return VerticalAlignment::Center;
      }

      return VerticalAlignment::Stretch;
    }
  } // namespace

  PlacementPlan planPlacement(uimodel::LayoutNode const& node)
  {
    auto const placement = uimodel::planLayoutPlacement(node);

    return PlacementPlan{
      .horizontalSlot = slotSizing(placement.optHorizontalExpand),
      .verticalSlot = slotSizing(placement.optVerticalExpand),
      .optHorizontalAlignment = placement.optHorizontalAlignment.transform(toHorizontal),
      .optVerticalAlignment = placement.optVerticalAlignment.transform(toVertical),
      .optMinWidth = placement.widthRequestAuthored ? std::optional{placement.optMinWidth.value_or(0.0)} : std::nullopt,
      .optMinHeight =
        placement.heightRequestAuthored ? std::optional{placement.optMinHeight.value_or(0.0)} : std::nullopt,
      .authoredVisible = placement.optAuthoredVisible.value_or(true),
    };
  }
} // namespace ao::winui
