// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/TuiHitRegions.h"

#include "tui/NotificationCenterPanel.h"
#include "tui/OutputDevicePanel.h"
#include "tui/PresentationPanel.h"
#include "tui/TrackTable.h"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/screen/box.hpp>

namespace ao::tui::test
{
  TEST_CASE("TuiHitRegions - hitTestButton resolves clickable buttons", "[tui][unit][hit-region]")
  {
    auto regions = TuiHitRegions{};
    regions.outputDeviceButtonBox = ftxui::Box{.x_min = 1, .x_max = 5, .y_min = 0, .y_max = 0};
    regions.soulButtonBox = ftxui::Box{.x_min = 6, .x_max = 8, .y_min = 0, .y_max = 0};
    regions.libraryButtonBox = ftxui::Box{.x_min = 0, .x_max = 9, .y_min = 10, .y_max = 10};
    regions.presentationButtonBox = ftxui::Box{.x_min = 10, .x_max = 19, .y_min = 10, .y_max = 10};
    regions.activityStatusBox = ftxui::Box{.x_min = 20, .x_max = 39, .y_min = 10, .y_max = 10};

    CHECK(regions.hitTestButton(2, 0).hoveredButton == HoveredButton::OutputDevice);
    CHECK(regions.hitTestButton(2, 0).isQualityHoverVisible == false);

    auto const soulHit = regions.hitTestButton(7, 0);
    CHECK(soulHit.hoveredButton == HoveredButton::Soul);
    CHECK(soulHit.isQualityHoverVisible == true);

    CHECK(regions.hitTestButton(2, 10).hoveredButton == HoveredButton::Library);
    CHECK(regions.hitTestButton(12, 10).hoveredButton == HoveredButton::Presentation);
    CHECK(regions.hitTestButton(30, 10).hoveredButton == HoveredButton::ActivityStatus);
    CHECK(regions.hitTestButton(60, 10).hoveredButton == HoveredButton::None);
  }

  TEST_CASE("TuiHitRegions - command and overlay context applies modal hover policy", "[tui][unit][hit-region]")
  {
    auto regions = TuiHitRegions{};
    regions.outputDeviceButtonBox = ftxui::Box{.x_min = 1, .x_max = 5, .y_min = 0, .y_max = 0};
    regions.soulButtonBox = ftxui::Box{.x_min = 6, .x_max = 8, .y_min = 0, .y_max = 0};

    auto const commandHit =
      regions.hitTestButton(2, 0, HitTestContext{.isCommandActive = true, .isOverlayActive = false});
    CHECK(commandHit.hoveredButton == HoveredButton::None);
    CHECK(commandHit.isQualityHoverVisible == false);

    auto const overlaySoulHit =
      regions.hitTestButton(7, 0, HitTestContext{.isCommandActive = false, .isOverlayActive = true});
    CHECK(overlaySoulHit.hoveredButton == HoveredButton::Soul);
    CHECK(overlaySoulHit.isQualityHoverVisible == false);
  }

  TEST_CASE("TuiHitRegions - clearFrameLocalRows keeps persistent button boxes", "[tui][unit][hit-region]")
  {
    auto regions = TuiHitRegions{};
    regions.libraryButtonBox = ftxui::Box{.x_min = 1, .x_max = 4, .y_min = 2, .y_max = 2};
    regions.outputDeviceRows.push_back(OutputDeviceRowBox{.rowIndex = 1});
    regions.presentationRows.push_back(PresentationRowBox{.rowIndex = 2});
    regions.notificationDetailRows.push_back(NotificationDetailRowBox{.dismissible = true});
    regions.trackColumnResizeHandles.push_back(TrackColumnResizeHandle{});
    regions.trackSectionRows.push_back(TrackSectionRowBox{.sectionIndex = 3});

    regions.clearFrameLocalRows();

    CHECK(regions.outputDeviceRows.empty());
    CHECK(regions.presentationRows.empty());
    CHECK(regions.notificationDetailRows.empty());
    CHECK(regions.trackColumnResizeHandles.empty());
    CHECK(regions.trackSectionRows.empty());
    CHECK(regions.libraryButtonBox.x_min == 1);
    CHECK(regions.libraryButtonBox.x_max == 4);
  }
} // namespace ao::tui::test
