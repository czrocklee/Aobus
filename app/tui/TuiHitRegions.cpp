// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TuiHitRegions.h"

#include <ftxui/screen/box.hpp>

#include <cstdint>

namespace ao::tui
{
  bool hasHitArea(ftxui::Box const& box)
  {
    return box.x_min <= box.x_max && box.y_min <= box.y_max &&
           (box.x_min != 0 || box.x_max != 0 || box.y_min != 0 || box.y_max != 0);
  }

  bool contains(ftxui::Box const& box, std::int32_t const column, std::int32_t const row)
  {
    return hasHitArea(box) && column >= box.x_min && column <= box.x_max && row >= box.y_min && row <= box.y_max;
  }

  void TuiHitRegions::clearFrameLocalRows()
  {
    outputDeviceRows.clear();
    presentationRows.clear();
    notificationDetailRows.clear();
    trackColumnResizeHandles.clear();
    trackSectionRows.clear();
  }

  ButtonHitTestResult TuiHitRegions::hitTestButton(std::int32_t const column,
                                                   std::int32_t const row,
                                                   HitTestContext const context) const
  {
    if (context.commandActive)
    {
      return {};
    }

    auto result = ButtonHitTestResult{};

    if (contains(outputDeviceButtonBox, column, row))
    {
      result.hoveredButton = HoveredButton::OutputDevice;
      return result;
    }

    if (contains(soulButtonBox, column, row))
    {
      result.hoveredButton = HoveredButton::Soul;
      result.qualityHoverVisible = !context.overlayActive;
      return result;
    }

    if (contains(libraryButtonBox, column, row))
    {
      result.hoveredButton = HoveredButton::Library;
      return result;
    }

    if (contains(presentationButtonBox, column, row))
    {
      result.hoveredButton = HoveredButton::Presentation;
      return result;
    }

    if (contains(activityStatusBox, column, row))
    {
      result.hoveredButton = HoveredButton::ActivityStatus;
      return result;
    }

    return result;
  }
} // namespace ao::tui
