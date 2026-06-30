// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/LibraryController.h"

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/CorePrimitives.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace ao::tui::test
{
  namespace
  {
    struct LibraryControllerFixture final
    {
      ao::test::TempDir tempDir{};
      rt::AppRuntime runtime{rt::test::makeRuntime(tempDir)};

      TrackId addTrack(std::string_view title)
      {
        return library::test::addTrack(runtime.musicLibrary(), library::test::TrackSpec{.title = std::string{title}});
      }
    };
  } // namespace

  TEST_CASE("LibraryController - startup publishes an active all-tracks view", "[tui][regression][library]")
  {
    auto fixture = LibraryControllerFixture{};
    auto const trackId = fixture.addTrack("Needle");

    auto controller = LibraryController{fixture.runtime};

    REQUIRE(controller.activeViewId() != rt::kInvalidViewId);
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks()[0].id == trackId);

    controller.setFilterDraft("Needle");
    CHECK(controller.applyFilter() == "Quick filter matched 1 tracks");
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks()[0].id == trackId);
  }

  TEST_CASE("LibraryController - reload clears stale filter draft", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("Needle");
    fixture.addTrack("Other");

    auto controller = LibraryController{fixture.runtime};
    controller.setFilterDraft("Needle");
    CHECK(controller.applyFilter() == "Quick filter matched 1 tracks");
    REQUIRE(controller.tracks().size() == 1);

    CHECK(controller.reloadActiveList() == "Reloaded 2 tracks");

    CHECK(controller.filterDraft().empty());
    CHECK(controller.tracks().size() == 2);
  }

  TEST_CASE("LibraryController - expression filters report expression mode", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    auto const trackId = fixture.addTrack("Needle");
    fixture.addTrack("Other");

    auto controller = LibraryController{fixture.runtime};
    controller.setFilterDraft("$title ~ \"Needle\"");

    CHECK(controller.applyFilter() == "Expression filter matched 1 tracks");
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks()[0].id == trackId);
  }

  TEST_CASE("LibraryController - empty track views have no selected track", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("Needle");

    auto controller = LibraryController{fixture.runtime};
    controller.setFilterDraft("not-present");

    CHECK(controller.applyFilter() == "Quick filter matched 0 tracks");
    CHECK(controller.tracks().empty());

    auto selected = controller.selectedTrackView();
    CHECK(selected.track == nullptr);
    CHECK(selected.coverArtId == kInvalidResourceId);
  }
} // namespace ao::tui::test
