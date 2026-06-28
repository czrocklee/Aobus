// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/uimodel/library/track/TrackFilterViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  namespace
  {
    struct TrackFilterFixture final
    {
      TestMusicLibrary testLib;
      MockExecutor executor;
      LibraryChanges changes;
      ListSourceStore listSourceStore{testLib.library(), changes};
      ViewService viewService{executor, testLib.library(), listSourceStore};
      PlaybackService playback{executor, viewService, testLib.library()};
      WorkspaceService workspaceService{viewService, playback, changes, testLib.library()};
      RenderLog<TrackFilterViewState> renderLog;
      TrackFilterViewModel viewModel{viewService,
                                     workspaceService,
                                     [this](auto const& view) { renderLog.render(view); }};

      rt::ViewId focusAllTracksView()
      {
        auto reply = viewService.createView(rt::TrackListViewConfig{.listId = rt::kAllTracksListId});
        workspaceService.setFocusedView(reply.viewId);
        return reply.viewId;
      }
    };
  } // namespace

  TEST_CASE("TrackFilterViewModel - initial render produces disabled state", "[uimodel][unit][track_filter]")
  {
    auto fixture = TrackFilterFixture{};

    REQUIRE(!fixture.renderLog.empty());
    CHECK(fixture.renderLog.last().enabled == false);
    CHECK(fixture.renderLog.last().entryText.empty());
  }

  TEST_CASE("TrackFilterViewModel - empty filter text keeps creation disabled", "[uimodel][unit][track_filter]")
  {
    auto fixture = TrackFilterFixture{};

    fixture.viewModel.updateFilter("");

    CHECK(fixture.renderLog.last().enabled == false);
    CHECK(fixture.renderLog.last().entryText.empty());
    CHECK(fixture.renderLog.last().resolvedExpression.empty());
    CHECK(fixture.renderLog.last().pending == false);
    CHECK(fixture.renderLog.last().canCreateSmartList == false);
  }

  TEST_CASE("TrackFilterViewModel - expression syntax becomes the resolved expression", "[uimodel][unit][track_filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();

    fixture.viewModel.updateFilter("$artist ~ 'Beatles'");

    CHECK(fixture.renderLog.last().enabled == true);
    CHECK(fixture.renderLog.last().entryText == "$artist ~ 'Beatles'");
    CHECK(fixture.renderLog.last().resolvedExpression == "$artist ~ 'Beatles'");
    CHECK(fixture.renderLog.last().pending == false);
    CHECK(fixture.renderLog.last().hasError == false);
    CHECK(fixture.renderLog.last().canCreateSmartList == true);
  }

  TEST_CASE("TrackFilterViewModel - plain text resolves to quick search expression", "[uimodel][unit][track_filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();

    fixture.viewModel.updateFilter("Beatles");

    CHECK(fixture.renderLog.last().entryText == "Beatles");
    CHECK(fixture.renderLog.last().resolvedExpression.contains("$title ~ \"Beatles\""));
    CHECK(fixture.renderLog.last().resolvedExpression.contains("$artist ~ \"Beatles\""));
    CHECK(fixture.renderLog.last().canCreateSmartList == true);
  }

  TEST_CASE("TrackFilterViewModel - multiple plain text terms resolve to conjunction", "[uimodel][unit][track_filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();

    fixture.viewModel.updateFilter("Beatles help");

    CHECK(fixture.renderLog.last().entryText == "Beatles help");
    CHECK(fixture.renderLog.last().resolvedExpression.contains(") and ("));
    CHECK(fixture.renderLog.last().canCreateSmartList == true);
  }

  TEST_CASE("TrackFilterViewModel - focused track view enables filtering", "[uimodel][unit][track_filter]")
  {
    auto fixture = TrackFilterFixture{};
    auto config = rt::TrackListViewConfig{
      .listId = rt::kAllTracksListId, .filterExpression = {}, .groupBy = rt::TrackGroupKey::None};

    auto reply = fixture.viewService.createView(config);
    fixture.workspaceService.setFocusedView(reply.viewId);

    REQUIRE(!fixture.renderLog.empty());
    CHECK(fixture.renderLog.last().enabled == true);
  }

  TEST_CASE("TrackFilterViewModel - filter edits update the focused view", "[uimodel][unit][track_filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();

    fixture.viewModel.updateFilter("$artist ~ 'Beatles'");

    CHECK(fixture.renderLog.last().entryText == "$artist ~ 'Beatles'");
    CHECK(fixture.renderLog.last().resolvedExpression == "$artist ~ 'Beatles'");
    CHECK(fixture.renderLog.last().pending == false);
    CHECK(fixture.renderLog.last().hasError == false);
    CHECK(fixture.renderLog.last().canCreateSmartList == true);
  }

  TEST_CASE("TrackFilterViewModel - filter edits preserve focused view presentation", "[uimodel][unit][track_filter]")
  {
    auto fixture = TrackFilterFixture{};
    auto config = rt::TrackListViewConfig{.listId = rt::kAllTracksListId};
    config.optPresentation = rt::defaultTrackPresentationSpec();
    config.optPresentation->id = "custom";
    auto reply = fixture.viewService.createView(config);
    fixture.workspaceService.setFocusedView(reply.viewId);

    fixture.viewModel.updateFilter("artist == 'Muse'");

    auto const state = fixture.viewService.trackListState(reply.viewId);
    CHECK(state.filterExpression == "artist == 'Muse'");
    CHECK(state.presentation.id == "custom");
    CHECK(fixture.renderLog.last().entryText == "artist == 'Muse'");
    CHECK(fixture.renderLog.last().resolvedExpression == "artist == 'Muse'");
  }

  TEST_CASE("TrackFilterViewModel - quoted plain text is escaped in quick search", "[uimodel][unit][track_filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();

    fixture.viewModel.updateFilter("\"A Song Name\"");

    CHECK(fixture.renderLog.last().entryText == "\"A Song Name\"");
    CHECK(fixture.renderLog.last().resolvedExpression.contains("\"A Song Name\""));
    CHECK(fixture.renderLog.last().canCreateSmartList == true);
  }

  TEST_CASE("TrackFilterViewModel - losing focus clears filter state", "[uimodel][unit][track_filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();
    fixture.viewModel.updateFilter("Beatles");

    fixture.workspaceService.setFocusedView(rt::kInvalidViewId);
    CHECK(fixture.renderLog.last().enabled == false);
    CHECK(fixture.renderLog.last().entryText.empty());
  }
} // namespace ao::uimodel::test
