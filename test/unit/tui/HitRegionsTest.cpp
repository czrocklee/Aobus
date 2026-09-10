// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/HitRegions.h"

#include "tui/NotificationCenterPanel.h"
#include "tui/OutputDevicePanel.h"
#include "tui/PresentationPanel.h"
#include "tui/TrackTable.h"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/screen/box.hpp>

namespace ao::tui::test
{
  TEST_CASE("HitRegions - hitTestButton resolves clickable buttons", "[tui][unit][hit-region]")
  {
    auto regions = HitRegions{};
    regions.outputDeviceButtonBox = ftxui::Box{.x_min = 1, .x_max = 5, .y_min = 0, .y_max = 0};
    regions.soulButtonBox = ftxui::Box{.x_min = 6, .x_max = 8, .y_min = 0, .y_max = 0};
    regions.presentationButtonBox = ftxui::Box{.x_min = 10, .x_max = 19, .y_min = 10, .y_max = 10};
    regions.activityStatusBox = ftxui::Box{.x_min = 20, .x_max = 39, .y_min = 10, .y_max = 10};
    regions.libraryButtonBox = {.x_min = 1, .x_max = 5, .y_min = 10, .y_max = 10};

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

  TEST_CASE("HitRegions - text-input and overlay context applies modal hover policy", "[tui][unit][hit-region]")
  {
    auto regions = HitRegions{};
    regions.outputDeviceButtonBox = ftxui::Box{.x_min = 1, .x_max = 5, .y_min = 0, .y_max = 0};
    regions.soulButtonBox = ftxui::Box{.x_min = 6, .x_max = 8, .y_min = 0, .y_max = 0};

    auto const inputHit =
      regions.hitTestButton(2, 0, HitTestContext{.isTextInputActive = true, .isOverlayActive = false});
    CHECK(inputHit.hoveredButton == HoveredButton::None);
    CHECK(inputHit.isQualityHoverVisible == false);

    auto const overlaySoulHit =
      regions.hitTestButton(7, 0, HitTestContext{.isTextInputActive = false, .isOverlayActive = true});
    CHECK(overlaySoulHit.hoveredButton == HoveredButton::Soul);
    CHECK(overlaySoulHit.isQualityHoverVisible == false);
  }

  TEST_CASE("HitRegions - clearFrameLocalRows clears conditional footer buttons", "[tui][unit][hit-region]")
  {
    auto regions = HitRegions{};
    regions.presentationButtonBox = ftxui::Box{.x_min = 1, .x_max = 4, .y_min = 2, .y_max = 2};
    regions.shuffleBox = {.x_min = 20, .x_max = 20, .y_min = 0, .y_max = 0};
    regions.repeatBox = {.x_min = 22, .x_max = 23, .y_min = 0, .y_max = 0};
    regions.outputDeviceRows.push_back(OutputDeviceRowHitRegion{.rowIndex = 1});
    regions.presentationRows.push_back(PresentationRowHitRegion{.rowIndex = 2});
    regions.notificationDetailRows.push_back(NotificationDetailRowHitRegion{.dismissible = true});
    regions.trackColumnResizeHandles.push_back(TrackColumnResizeHandle{});
    regions.trackSectionRows.push_back(TrackSectionRowHitRegion{.sectionIndex = 3});

    regions.clearFrameLocalRows();

    CHECK(regions.shuffleBox.IsEmpty());
    CHECK(regions.repeatBox.IsEmpty());
    CHECK(regions.outputDeviceRows.empty());
    CHECK(regions.presentationRows.empty());
    CHECK(regions.notificationDetailRows.empty());
    CHECK(regions.trackColumnResizeHandles.empty());
    CHECK(regions.trackSectionRows.empty());
    CHECK(regions.presentationButtonBox.IsEmpty());
    CHECK(regions.hitTestButton(2, 2, {}).hoveredButton == HoveredButton::None);
  }
} // namespace ao::tui::test
