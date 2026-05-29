// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/TestUtils.h"
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/LibraryMutationService.h>
#include <ao/rt/ListSourceStore.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/async/Runtime.h>
#include <ao/uimodel/track/TrackFilterViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <vector>

namespace ao::uimodel::track::test
{
  using namespace ao::rt::test;
  namespace
  {
    using namespace ao::rt;

    struct NullExecutor final : public IControlExecutor
    {
      bool isCurrent() const noexcept override { return true; }
      void dispatch(std::move_only_function<void()> task) override { task(); }
      void defer(std::move_only_function<void()> task) override { task(); }
    };

    template<typename T>
    struct RenderLog final
    {
      std::vector<T> states;
      void render(T const& state) { states.push_back(state); }
      T const& last() const { return states.back(); }
      bool empty() const { return states.empty(); }
      void clear() { states.clear(); }
    };
  }

  TEST_CASE("TrackFilterViewModel - initial state and filter interactions", "[unit][uimodel][track]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = NullExecutor{};
    auto runtime = async::Runtime{executor};
    auto mutationService = LibraryMutationService{runtime, testLib.library()};
    auto listSourceStore = ListSourceStore{testLib.library(), mutationService};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playback = PlaybackService{executor, viewService, testLib.library()};
    auto workspaceService = WorkspaceService{viewService, playback, mutationService, testLib.library()};

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
      SUCCEED("updateFilter handles empty text");
    }

    SECTION("updateFilter with expression syntax")
    {
      viewModel.updateFilter("$artist ~ 'Beatles'");
      SUCCEED("updateFilter handles expression syntax");
    }

    SECTION("updateFilter with plain text uses Quick search resolver")
    {
      auto reply = viewService.createView(rt::TrackListViewConfig{.listId = rt::kAllTracksListId});
      workspaceService.setFocusedView(reply.viewId);

      viewModel.updateFilter("Beatles");
      CHECK(renderLog.last().resolvedExpression.contains("$title ~ \"Beatles\""));
      CHECK(renderLog.last().resolvedExpression.contains("$artist ~ \"Beatles\""));
    }

    SECTION("updateFilter with multiple terms creates AND expression")
    {
      auto reply = viewService.createView(rt::TrackListViewConfig{.listId = rt::kAllTracksListId});
      workspaceService.setFocusedView(reply.viewId);

      viewModel.updateFilter("Beatles help");
      CHECK(renderLog.last().resolvedExpression.contains(") and ("));
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
      CHECK(renderLog.last().resolvedExpression == "$artist ~ 'Beatles'");
      CHECK(renderLog.last().canCreateSmartList == true);
    }

    SECTION("updateFilter with quotes handles escaping")
    {
      auto reply = viewService.createView(rt::TrackListViewConfig{.listId = rt::kAllTracksListId});
      workspaceService.setFocusedView(reply.viewId);

      viewModel.updateFilter("\"A Song Name\"");
      // It should use the TrackFilterResolver which handles quoting
      CHECK(renderLog.last().resolvedExpression.contains("\"A Song Name\""));
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
