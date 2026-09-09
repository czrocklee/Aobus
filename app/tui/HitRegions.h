// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CommandPalettePanel.h"
#include "MouseBindings.h"
#include "NavigationPanel.h"
#include "NotificationCenterPanel.h"
#include "OutputDevicePanel.h"
#include "PresentationPanel.h"
#include "StatusBar.h"
#include "TrackTable.h"

#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <list>
#include <vector>

namespace ao::tui
{
  bool hasCoverIntersection(ftxui::Box const& cover, ftxui::Box const& foreground);

  enum class HoveredButton : std::uint8_t
  {
    None,
    OutputDevice,
    Library,
    Soul,
    Presentation,
    ActivityStatus,
    Settings,
  };

  struct HitTestContext final
  {
    bool isTextInputActive = false;
    bool isOverlayActive = false;
  };

  struct ButtonHitTestResult final
  {
    HoveredButton hoveredButton = HoveredButton::None;
    bool isQualityHoverVisible = false;
  };

  bool hasHitArea(ftxui::Box const& box);
  bool contains(ftxui::Box const& box, std::int32_t column, std::int32_t row);

  struct HitRegions final
  {
    ftxui::Box coverBox{};
    ftxui::Box libraryButtonBox{};
    ftxui::Box soulButtonBox{};
    ftxui::Box outputDeviceButtonBox{};
    ftxui::Box presentationButtonBox{};
    ftxui::Box activityStatusBox{};
    ftxui::Box settingsButtonBox{};
    ftxui::Box cancelSelectionBox = kEmptyMouseBox;
    ftxui::Box seekRailBox{};
    ftxui::Box volumeBox{};
    ftxui::Box shuffleBox = kEmptyMouseBox;
    ftxui::Box repeatBox = kEmptyMouseBox;
    NavigationGeometry navigationLayout{};
    NavigationHitRegions navigation{};
    PanelMouseRegions overlayPanel{};
    PanelMouseRegions inputPanel{};
    CompletionHitRegions completion{};
    ftxui::Box trackTableBox = kEmptyMouseBox;
    /// Materialized rows that supplied the painted section and resize targets.
    std::uint64_t trackTableRevision = 0;

    std::list<StatusActionHitRegion> statusActions{};
    std::vector<TrackRowHitRegion> trackRows{};
    std::vector<OutputDeviceRowHitRegion> outputDeviceRows{};
    std::vector<PresentationRowHitRegion> presentationRows{};
    std::vector<NotificationDetailRowHitRegion> notificationDetailRows{};
    std::vector<TrackColumnResizeHandle> trackColumnResizeHandles{};
    std::vector<TrackSectionRowHitRegion> trackSectionRows{};

    void clearFrameLocalRows();
    ButtonHitTestResult hitTestButton(std::int32_t column, std::int32_t row, HitTestContext context = {}) const;
  };
} // namespace ao::tui
