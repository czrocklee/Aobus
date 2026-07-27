// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/library/track/TrackFilterViewModel.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  namespace
  {
    struct TrackFilterFixture final
    {
      MusicLibraryFixture libraryFixture;
      InlineExecutor executor;
      LibraryChanges changes{executor, 0};
      TrackSourceCache trackSourceCache{libraryFixture.library(), changes};
      ViewService viewService{executor, libraryFixture.library(), trackSourceCache};
      WorkspaceService workspaceService{executor, viewService, changes};
      ao::test::RenderLog<TrackFilterViewState> renderLog;
      TrackFilterViewModel viewModel{viewService,
                                     workspaceService,
                                     [this](auto const& view) { renderLog.render(view); }};

      rt::ViewId focusAllTracksView()
      {
        return ao::test::requireValue(workspaceService.navigate({.target = rt::GlobalViewKind::AllTracks}));
      }
    };
  } // namespace

  TEST_CASE("TrackFilterViewModel - initial render produces disabled state", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};

    REQUIRE(!fixture.renderLog.empty());
    CHECK(fixture.renderLog.last().enabled == false);
    CHECK(fixture.renderLog.last().entryText.empty());
  }

  TEST_CASE("TrackFilterViewModel - empty filter text keeps creation disabled", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};

    fixture.viewModel.updateFilter("");

    CHECK(fixture.renderLog.last().enabled == false);
    CHECK(fixture.renderLog.last().entryText.empty());
    CHECK(fixture.renderLog.last().resolvedExpression.empty());
    CHECK(fixture.renderLog.last().canCreateSmartList == false);
  }

  TEST_CASE("TrackFilterViewModel - expression syntax becomes the resolved expression", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();

    fixture.viewModel.updateFilter("$artist ~ 'Beatles'");

    CHECK(fixture.renderLog.last().enabled == true);
    CHECK(fixture.renderLog.last().entryText == "$artist ~ 'Beatles'");
    CHECK(fixture.renderLog.last().resolvedExpression == "$artist ~ 'Beatles'");
    CHECK(fixture.renderLog.last().hasError == false);
    CHECK(fixture.renderLog.last().canCreateSmartList == true);
  }

  TEST_CASE("TrackFilterViewModel - plain text resolves to quick search expression",
            "[uimodel][unit][track-filter][regression]")
  {
    auto fixture = TrackFilterFixture{};
    auto const aimerTrackId =
      fixture.libraryFixture.addTrack(library::test::TrackSpec{.title = "Brave Shine", .artist = "Aimer"});
    fixture.libraryFixture.addTrack(library::test::TrackSpec{.title = "Hysteria", .artist = "Muse"});
    fixture.trackSourceCache.reloadAllTracks();
    auto const viewId = fixture.focusAllTracksView();

    fixture.viewModel.updateFilter("Aimer");

    CHECK(fixture.renderLog.last().entryText == "Aimer");
    CHECK(fixture.renderLog.last().resolvedExpression.contains("$title ~ \"Aimer\""));
    CHECK(fixture.renderLog.last().resolvedExpression.contains("$artist ~ \"Aimer\""));
    CHECK(fixture.renderLog.last().canCreateSmartList == true);
    auto const projectionPtr = ao::test::requireValue(fixture.viewService.findTrackListProjection(viewId));
    REQUIRE(projectionPtr != nullptr);
    REQUIRE(projectionPtr->size() == 1);
    CHECK(projectionPtr->trackIdAt(0) == aimerTrackId);
    CHECK_FALSE(fixture.viewService.trackListState(viewId).optFilterError);

    fixture.viewModel.updateFilter("");

    auto const clearedProjectionPtr = ao::test::requireValue(fixture.viewService.findTrackListProjection(viewId));
    REQUIRE(clearedProjectionPtr != nullptr);
    REQUIRE(clearedProjectionPtr->size() == 2);
  }

  TEST_CASE("TrackFilterViewModel - multiple plain text terms resolve to conjunction", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();

    fixture.viewModel.updateFilter("Beatles help");

    CHECK(fixture.renderLog.last().entryText == "Beatles help");
    CHECK(fixture.renderLog.last().resolvedExpression.contains(") and ("));
    CHECK(fixture.renderLog.last().canCreateSmartList == true);
  }

  TEST_CASE("TrackFilterViewModel - focused track view enables filtering", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    REQUIRE(fixture.workspaceService.navigate({.target = rt::GlobalViewKind::AllTracks}));

    REQUIRE(!fixture.renderLog.empty());
    CHECK(fixture.renderLog.last().enabled == true);
  }

  TEST_CASE("TrackFilterViewModel - filter edits update the focused view", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();
    fixture.renderLog.clear();

    fixture.viewModel.updateFilter("$artist ~ 'Beatles'");

    REQUIRE(fixture.renderLog.states.size() == 1);
    CHECK(fixture.renderLog.last().entryText == "$artist ~ 'Beatles'");
    CHECK(fixture.renderLog.last().resolvedExpression == "$artist ~ 'Beatles'");
    CHECK(fixture.renderLog.last().hasError == false);
    CHECK(fixture.renderLog.last().canCreateSmartList == true);
  }

  TEST_CASE("TrackFilterViewModel - invalid expression exposes the runtime error", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();

    fixture.viewModel.updateFilter("$year >");

    CHECK(fixture.renderLog.last().entryText == "$year >");
    CHECK(fixture.renderLog.last().resolvedExpression == "$year >");
    CHECK(fixture.renderLog.last().hasError == true);
    CHECK(fixture.renderLog.last().tooltip.contains("Filter error"));
    CHECK(fixture.renderLog.last().canCreateSmartList == false);
  }

  TEST_CASE("TrackFilterViewModel - filter edits preserve focused view presentation", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    auto config = rt::TrackListViewConfig{.listId = rt::kAllTracksListId};
    config.optPresentation = rt::defaultTrackPresentationSpec();
    config.optPresentation->id = "custom";
    auto const viewId = ao::test::requireValue(fixture.workspaceService.navigate({
      .target = rt::GlobalViewKind::AllTracks,
      .optPresentation =
        rt::NavigationPresentation{
          .mode = rt::NavigationPresentationMode::Override,
          .spec = *config.optPresentation,
        },
    }));

    fixture.viewModel.updateFilter("$artist = \"Muse\"");

    auto const state = fixture.viewService.trackListState(viewId);
    CHECK(state.filterExpression == "$artist = \"Muse\"");
    CHECK(state.presentation.id == "custom");
    CHECK(fixture.renderLog.last().entryText == "$artist = \"Muse\"");
    CHECK(fixture.renderLog.last().resolvedExpression == "$artist = \"Muse\"");
  }

  TEST_CASE("TrackFilterViewModel - quoted plain text is escaped in quick search", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();

    fixture.viewModel.updateFilter("\"A Song Name\"");

    CHECK(fixture.renderLog.last().entryText == "\"A Song Name\"");
    CHECK(fixture.renderLog.last().resolvedExpression.contains("\"A Song Name\""));
    CHECK(fixture.renderLog.last().canCreateSmartList == true);
  }

  TEST_CASE("TrackFilterViewModel - losing focus clears filter state", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();
    fixture.viewModel.updateFilter("Beatles");

    REQUIRE(fixture.workspaceService.closeView(fixture.workspaceService.snapshot().activeViewId));
    CHECK(fixture.renderLog.last().enabled == false);
    CHECK(fixture.renderLog.last().entryText.empty());
  }
} // namespace ao::uimodel::test
