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
#include <ao/uimodel/track/TrackFilterViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>

namespace ao::uimodel::track::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  TEST_CASE("TrackFilterViewModel - initial state and filter interactions", "[unit][uimodel][track]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playback = PlaybackService{executor, viewService, testLib.library()};
    auto workspaceService = WorkspaceService{viewService, playback, changes, testLib.library()};

    auto renderLog = RenderLog<TrackFilterViewState>{};
    auto viewModel =
      TrackFilterViewModel{viewService, workspaceService, [&renderLog](auto const& view) { renderLog.render(view); }};

    SECTION("Initial render produces enabled view state")
    {
      REQUIRE(!renderLog.empty());
      CHECK(renderLog.last().enabled == false);
      CHECK(renderLog.last().entryText.empty());
    }

    SECTION("updateFilter with empty text")
    {
      viewModel.updateFilter("");

      CHECK(renderLog.last().enabled == false);
      CHECK(renderLog.last().entryText.empty());
      CHECK(renderLog.last().resolvedExpression.empty());
      CHECK(renderLog.last().pending == false);
      CHECK(renderLog.last().canCreateSmartList == false);
    }

    SECTION("updateFilter with expression syntax")
    {
      auto reply = viewService.createView(rt::TrackListViewConfig{.listId = rt::kAllTracksListId});
      workspaceService.setFocusedView(reply.viewId);

      viewModel.updateFilter("$artist ~ 'Beatles'");

      CHECK(renderLog.last().enabled == true);
      CHECK(renderLog.last().entryText == "$artist ~ 'Beatles'");
      CHECK(renderLog.last().resolvedExpression == "$artist ~ 'Beatles'");
      CHECK(renderLog.last().pending == false);
      CHECK(renderLog.last().hasError == false);
      CHECK(renderLog.last().canCreateSmartList == true);
    }

    SECTION("updateFilter with plain text uses Quick search resolver")
    {
      auto reply = viewService.createView(rt::TrackListViewConfig{.listId = rt::kAllTracksListId});
      workspaceService.setFocusedView(reply.viewId);

      viewModel.updateFilter("Beatles");
      CHECK(renderLog.last().entryText == "Beatles");
      CHECK(renderLog.last().resolvedExpression.contains("$title ~ \"Beatles\""));
      CHECK(renderLog.last().resolvedExpression.contains("$artist ~ \"Beatles\""));
      CHECK(renderLog.last().canCreateSmartList == true);
    }

    SECTION("updateFilter with multiple terms creates AND expression")
    {
      auto reply = viewService.createView(rt::TrackListViewConfig{.listId = rt::kAllTracksListId});
      workspaceService.setFocusedView(reply.viewId);

      viewModel.updateFilter("Beatles help");
      CHECK(renderLog.last().entryText == "Beatles help");
      CHECK(renderLog.last().resolvedExpression.contains(") and ("));
      CHECK(renderLog.last().canCreateSmartList == true);
    }

    SECTION("Focus on a view enables filter")
    {
      auto config = rt::TrackListViewConfig{
        .listId = rt::kAllTracksListId, .filterExpression = {}, .groupBy = rt::TrackGroupKey::None};
      auto reply = viewService.createView(config);
      workspaceService.setFocusedView(reply.viewId);

      REQUIRE(!renderLog.empty());
      CHECK(renderLog.last().enabled == true);
    }

    SECTION("updateFilter with view focused sets expression")
    {
      auto reply = viewService.createView(rt::TrackListViewConfig{.listId = rt::kAllTracksListId});
      workspaceService.setFocusedView(reply.viewId);

      viewModel.updateFilter("$artist ~ 'Beatles'");
      CHECK(renderLog.last().entryText == "$artist ~ 'Beatles'");
      CHECK(renderLog.last().resolvedExpression == "$artist ~ 'Beatles'");
      CHECK(renderLog.last().pending == false);
      CHECK(renderLog.last().hasError == false);
      CHECK(renderLog.last().canCreateSmartList == true);
    }

    SECTION("updateFilter preserves the focused view presentation")
    {
      auto config = rt::TrackListViewConfig{.listId = rt::kAllTracksListId};
      config.optPresentation = rt::defaultTrackPresentationSpec();
      config.optPresentation->id = "custom";
      auto reply = viewService.createView(config);
      workspaceService.setFocusedView(reply.viewId);

      viewModel.updateFilter("artist == 'Muse'");

      auto const state = viewService.trackListState(reply.viewId);
      CHECK(state.filterExpression == "artist == 'Muse'");
      CHECK(state.presentation.id == "custom");
      CHECK(renderLog.last().entryText == "artist == 'Muse'");
      CHECK(renderLog.last().resolvedExpression == "artist == 'Muse'");
    }

    SECTION("updateFilter with quotes handles escaping")
    {
      auto reply = viewService.createView(rt::TrackListViewConfig{.listId = rt::kAllTracksListId});
      workspaceService.setFocusedView(reply.viewId);

      viewModel.updateFilter("\"A Song Name\"");
      CHECK(renderLog.last().entryText == "\"A Song Name\"");
      CHECK(renderLog.last().resolvedExpression.contains("\"A Song Name\""));
      CHECK(renderLog.last().canCreateSmartList == true);
    }

    SECTION("Focus lost clears filter state")
    {
      auto reply = viewService.createView(rt::TrackListViewConfig{.listId = rt::kAllTracksListId});
      workspaceService.setFocusedView(reply.viewId);
      viewModel.updateFilter("Beatles");

      workspaceService.setFocusedView(rt::kInvalidViewId);
      CHECK(renderLog.last().enabled == false);
      CHECK(renderLog.last().entryText.empty());
    }
  }
} // namespace ao::uimodel::track::test
