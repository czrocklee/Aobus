// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "NotificationCenterPanel.h"
#include "OutputDevicePanel.h"
#include "PresentationPanel.h"
#include "TrackTable.h"

#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <vector>

namespace ao::tui
{
  enum class HoveredButton : std::uint8_t
  {
    None,
    OutputDevice,
    Library,
    Soul,
    Presentation,
    ActivityStatus,
  };

  struct HitTestContext final
  {
    bool isCommandActive = false;
    bool isOverlayActive = false;
  };

  struct ButtonHitTestResult final
  {
    HoveredButton hoveredButton = HoveredButton::None;
    bool isQualityHoverVisible = false;
  };

  bool hasHitArea(ftxui::Box const& box);
  bool contains(ftxui::Box const& box, std::int32_t column, std::int32_t row);

  struct TuiHitRegions final
  {
    ftxui::Box coverBox{};
    ftxui::Box libraryButtonBox{};
    ftxui::Box soulButtonBox{};
    ftxui::Box outputDeviceButtonBox{};
    ftxui::Box presentationButtonBox{};
    ftxui::Box activityStatusBox{};
    ftxui::Box seekRailBox{};
    ftxui::Box trackTableBox{};

    std::vector<OutputDeviceRowHitRegion> outputDeviceRows{};
    std::vector<PresentationRowHitRegion> presentationRows{};
    std::vector<NotificationDetailRowHitRegion> notificationDetailRows{};
    std::vector<TrackColumnResizeHandle> trackColumnResizeHandles{};
    std::vector<TrackSectionRowHitRegion> trackSectionRows{};

    void clearFrameLocalRows();
    ButtonHitTestResult hitTestButton(std::int32_t column, std::int32_t row, HitTestContext context = {}) const;
  };
} // namespace ao::tui
