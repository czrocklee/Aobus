// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/LibraryController.h"

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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
        return rt::test::addRuntimeTrack(runtime, library::test::TrackSpec{.title = std::string{title}});
      }

      TrackId addTrack(library::test::TrackSpec const& spec) { return rt::test::addRuntimeTrack(runtime, spec); }
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
    CHECK(controller.applyFilter() == "Quick filter matched 1 track");
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
    CHECK(controller.applyFilter() == "Quick filter matched 1 track");
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

    CHECK(controller.applyFilter() == "Expression filter matched 1 track");
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
    CHECK(controller.applyFilter() == "Quick filter matched 1 track");
    CHECK(controller.revealTrack(hiddenId) == "Current track is not in this view");
    CHECK(controller.selectedTrack() == 0);
  }

  TEST_CASE("LibraryController - revealTrack selects a track after presentation reorder", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    auto const lastId = fixture.addTrack(library::test::TrackSpec{.title = "B One",
                                                                  .artist = "Artist",
                                                                  .album = "Album B",
                                                                  .albumArtist = "Artist",
                                                                  .uri = "b1.flac",
                                                                  .year = 2021});
    auto const targetId = fixture.addTrack(library::test::TrackSpec{.title = "A One",
                                                                    .artist = "Artist",
                                                                    .album = "Album A",
                                                                    .albumArtist = "Artist",
                                                                    .uri = "a1.flac",
                                                                    .year = 2020});
    auto const secondId = fixture.addTrack(library::test::TrackSpec{.title = "A Two",
                                                                    .artist = "Artist",
                                                                    .album = "Album A",
                                                                    .albumArtist = "Artist",
                                                                    .uri = "a2.flac",
                                                                    .year = 2020});

    auto controller = LibraryController{fixture.runtime};
    REQUIRE(controller.setPresentation("albums") == "View: albums");
    REQUIRE(controller.tracks().size() == 3);
    CHECK(controller.tracks()[0].id == targetId);
    CHECK(controller.tracks()[1].id == secondId);
    CHECK(controller.tracks()[2].id == lastId);

    CHECK(controller.revealTrack(targetId) == "Revealed A One");
    CHECK(controller.selectedTrack() == 0);
    REQUIRE(controller.selectedTrackView().track != nullptr);
    CHECK(controller.selectedTrackView().track->id == targetId);
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

  TEST_CASE("LibraryController - presentation list follows custom preset changes", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("First");

    auto controller = LibraryController{fixture.runtime};
    auto customSpec = rt::defaultTrackPresentationSpec();
    customSpec.id = "custom-songs";

    auto const initialCount = controller.presentationEntries().size();
    REQUIRE(fixture.runtime.workspace().addCustomPreset(rt::CustomTrackPresentationPreset{
      .label = "Custom Songs",
      .basePresetId = "songs",
      .spec = customSpec,
    }));

    REQUIRE(controller.presentationEntries().size() == initialCount + 1);
    CHECK(controller.presentationEntries().back().id == "custom-songs");
    CHECK(controller.selectedPresentation() == 0);

    CHECK_FALSE(controller.setSelectedPresentation(-1));
    CHECK_FALSE(controller.setSelectedPresentation(static_cast<std::int32_t>(controller.presentationEntries().size())));
    REQUIRE(controller.setSelectedPresentation(static_cast<std::int32_t>(controller.presentationEntries().size()) - 1));

    CHECK(controller.selectSelectedPresentation() == "View: custom-songs");
    CHECK(fixture.runtime.views().trackListState(controller.activeViewId()).presentation.id == "custom-songs");
    CHECK(controller.selectedPresentation() == static_cast<std::int32_t>(controller.presentationEntries().size()) - 1);
  }

  TEST_CASE("LibraryController - setPresentation preserves selected track identity", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    auto const olderId = fixture.addTrack(library::test::TrackSpec{
      .title = "Older", .artist = "Same Artist", .album = "Z Album", .uri = "older.flac", .year = 2000});
    fixture.addTrack(library::test::TrackSpec{
      .title = "Newer", .artist = "Same Artist", .album = "A Album", .uri = "newer.flac", .year = 2025});

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

  TEST_CASE("LibraryController - album presentation exposes projection sections", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack(library::test::TrackSpec{
      .title = "A One", .artist = "Artist", .album = "Album A", .albumArtist = "Artist", .year = 2020});
    fixture.addTrack(library::test::TrackSpec{
      .title = "A Two", .artist = "Artist", .album = "Album A", .albumArtist = "Artist", .year = 2020});
    fixture.addTrack(library::test::TrackSpec{
      .title = "B One", .artist = "Artist", .album = "Album B", .albumArtist = "Artist", .year = 2021});

    auto controller = LibraryController{fixture.runtime};

    CHECK(controller.setPresentation("albums") == "View: albums");
    REQUIRE(controller.sections().size() == 2);
    CHECK(controller.sections()[0].primaryText == "Album A");
    CHECK(controller.sections()[0].secondaryText == "Artist");
    CHECK(controller.sections()[0].tertiaryText == "2020");
    CHECK(controller.sections()[0].rowBegin == 0);
    CHECK(controller.sections()[0].rowCount == 2);
    CHECK(controller.sections()[1].primaryText == "Album B");
    CHECK(controller.sections()[1].rowBegin == 2);
    CHECK(controller.sections()[1].rowCount == 1);
  }

  TEST_CASE("LibraryController - section jumps keep track selection flat", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack(
      library::test::TrackSpec{.title = "A One", .artist = "Artist", .album = "Album A", .albumArtist = "Artist"});
    fixture.addTrack(
      library::test::TrackSpec{.title = "A Two", .artist = "Artist", .album = "Album A", .albumArtist = "Artist"});
    fixture.addTrack(
      library::test::TrackSpec{.title = "B One", .artist = "Artist", .album = "Album B", .albumArtist = "Artist"});

    auto controller = LibraryController{fixture.runtime};
    REQUIRE(controller.setPresentation("albums") == "View: albums");
    REQUIRE(controller.sections().size() == 2);

    CHECK(controller.jumpToAdjacentSection(1) == "Section: Album B");
    CHECK(controller.selectedTrack() == 2);

    CHECK(controller.jumpToAdjacentSection(-1) == "Section: Album A");
    CHECK(controller.selectedTrack() == 0);
  }

  TEST_CASE("LibraryController - section selection reports empty and invalid states", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("First");

    auto controller = LibraryController{fixture.runtime};
    REQUIRE(controller.sections().empty());

    CHECK(controller.jumpToAdjacentSection(1) == "No sections in this view");
    CHECK(controller.selectSection(0) == "No section selected");
    CHECK(controller.selectSection(-1) == "No section selected");
    CHECK(controller.selectedTrack() == 0);
  }
} // namespace ao::tui::test
