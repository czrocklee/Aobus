// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/LibraryController.h"

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ViewService.h>

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

      TrackId addTrack(library::test::TrackSpec const& spec)
      {
        return library::test::addTrack(runtime.musicLibrary(), spec);
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

  TEST_CASE("LibraryController - revealTrack selects a visible track", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("First");
    auto const secondId = fixture.addTrack("Second");

    auto controller = LibraryController{fixture.runtime};

    CHECK(controller.revealTrack(secondId) == "Revealed Second");
    CHECK(controller.selectedTrack() == 1);
    REQUIRE(controller.selectedTrackView().track != nullptr);
    CHECK(controller.selectedTrackView().track->id == secondId);
  }

  TEST_CASE("LibraryController - revealTrack reports unavailable targets", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("First");
    auto const hiddenId = fixture.addTrack("Hidden");

    auto controller = LibraryController{fixture.runtime};

    CHECK(controller.revealTrack(kInvalidTrackId) == "No current track");

    controller.setFilterDraft("First");
    CHECK(controller.applyFilter() == "Quick filter matched 1 tracks");
    CHECK(controller.revealTrack(hiddenId) == "Current track is not in this view");
    CHECK(controller.selectedTrack() == 0);
  }

  TEST_CASE("LibraryController - setPresentation applies active workspace presentation", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("First");

    auto controller = LibraryController{fixture.runtime};

    CHECK(controller.setPresentation("albums") == "View: albums");
    CHECK(fixture.runtime.views().trackListState(controller.activeViewId()).presentation.id == "albums");
    CHECK(controller.setPresentation("missing-preset") == "Unknown view missing-preset");
    CHECK(fixture.runtime.views().trackListState(controller.activeViewId()).presentation.id == "albums");
  }

  TEST_CASE("LibraryController - setPresentation preserves selected track identity", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    auto const olderId = fixture.addTrack(library::test::TrackSpec{
      .title = "Older", .artist = "Same Artist", .album = "Z Album", .uri = "/tmp/older.flac", .year = 2000});
    fixture.addTrack(library::test::TrackSpec{
      .title = "Newer", .artist = "Same Artist", .album = "A Album", .uri = "/tmp/newer.flac", .year = 2025});

    auto controller = LibraryController{fixture.runtime};
    REQUIRE(controller.tracks().size() == 2);
    REQUIRE(controller.tracks()[1].id == olderId);

    controller.moveFocusedSelection(false, 1);
    REQUIRE(controller.selectedTrackView().track != nullptr);
    REQUIRE(controller.selectedTrackView().track->id == olderId);

    CHECK(controller.setPresentation("artists") == "View: artists");
    REQUIRE(controller.selectedTrackView().track != nullptr);
    CHECK(controller.selectedTrackView().track->id == olderId);
    CHECK(controller.selectedTrack() == 0);
  }
} // namespace ao::tui::test
