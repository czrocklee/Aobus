// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/WorkspaceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::test;

  TEST_CASE("WorkspaceService - navigate deduplicates the current list", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.firstListId);
    requireNavigation(runtime, fixture.secondListId);
    requireNavigation(runtime, fixture.secondListId);

    requireBackNavigation(runtime);
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.firstListId);
  }

  TEST_CASE("WorkspaceService - navigate request can skip recording history", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.firstListId);
    requireNavigation(runtime, NavigationRequest{.target = fixture.secondListId, .recordHistory = false});

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.secondListId);
    CHECK_FALSE(runtime.workspace().goBack());
  }

  TEST_CASE("WorkspaceService - navigate filtered target records filter history", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.firstListId);
    requireNavigation(runtime, FilteredListTarget{.listId = kAllTracksListId, .filterExpression = "genre == \"Rock\""});

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.filterExpression == "genre == \"Rock\"");

    requireBackNavigation(runtime);
    auto const backState = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(backState.listId == fixture.firstListId);
    CHECK(backState.filterExpression.empty());
  }

  TEST_CASE("WorkspaceService - goBack restores the previous list", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.firstListId);
    CHECK_FALSE(runtime.workspace().canGoBack());
    CHECK_FALSE(runtime.workspace().canGoForward());
    requireNavigation(runtime, fixture.secondListId);
    CHECK(runtime.workspace().canGoBack());
    CHECK_FALSE(runtime.workspace().canGoForward());
    requireNavigation(runtime, fixture.thirdListId);

    CHECK(runtime.workspace().goBack());
    CHECK(runtime.workspace().canGoBack());
    CHECK(runtime.workspace().canGoForward());
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.secondListId);
  }

  TEST_CASE("WorkspaceService - repeated goBack restores the first list", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.firstListId);
    requireNavigation(runtime, fixture.secondListId);
    requireNavigation(runtime, fixture.thirdListId);

    requireBackNavigation(runtime);
    requireBackNavigation(runtime);

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.firstListId);
    CHECK_FALSE(runtime.workspace().goBack());
  }

  TEST_CASE("WorkspaceService - goForward after back restores the newer list", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.firstListId);
    requireNavigation(runtime, fixture.secondListId);
    requireNavigation(runtime, fixture.thirdListId);
    requireBackNavigation(runtime);

    CHECK(runtime.workspace().goForward());
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.thirdListId);
    CHECK_FALSE(runtime.workspace().goForward());
  }

  TEST_CASE("WorkspaceService - goBack at the first entry reports NotFound without changing the workspace",
            "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.firstListId);
    auto const before = runtime.workspace().snapshot();
    std::int32_t changeCount = 0;
    auto const sub = runtime.workspace().onChanged([&](WorkspaceChanged const&) noexcept { ++changeCount; });

    auto const res = runtime.workspace().goBack();

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotFound);
    settleRuntimeCallbacks(runtime);
    CHECK(runtime.workspace().snapshot() == before);
    CHECK_FALSE(runtime.workspace().canGoBack());
    CHECK_FALSE(runtime.workspace().canGoForward());
    CHECK(changeCount == 0);
  }

  TEST_CASE("WorkspaceService - goForward at the newest entry reports NotFound without changing the workspace",
            "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.firstListId);
    auto const before = runtime.workspace().snapshot();
    std::int32_t changeCount = 0;
    auto const sub = runtime.workspace().onChanged([&](WorkspaceChanged const&) noexcept { ++changeCount; });

    auto const res = runtime.workspace().goForward();

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotFound);
    settleRuntimeCallbacks(runtime);
    CHECK(runtime.workspace().snapshot() == before);
    CHECK_FALSE(runtime.workspace().canGoBack());
    CHECK_FALSE(runtime.workspace().canGoForward());
    CHECK(changeCount == 0);
  }

  TEST_CASE("WorkspaceService - new navigation after back truncates forward history",
            "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.firstListId);
    requireNavigation(runtime, fixture.secondListId);
    requireNavigation(runtime, fixture.thirdListId);
    requireBackNavigation(runtime);
    requireNavigation(runtime, fixture.fourthListId);

    CHECK_FALSE(runtime.workspace().goForward());
    requireBackNavigation(runtime);
    auto const midState = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(midState.listId == fixture.secondListId);
    requireBackNavigation(runtime);
    auto const firstState = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(firstState.listId == fixture.firstListId);
  }

  TEST_CASE("WorkspaceService - goBack restores presentation state", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.firstListId);
    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    REQUIRE(runtime.workspace().setActivePresentation(albumsPreset->spec));

    requireBackNavigation(runtime);
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.presentation == normalizeTrackPresentationSpec(defaultTrackPresentationSpec()));
  }

  TEST_CASE("WorkspaceService - goBack works after closing the active view", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.firstListId);
    requireNavigation(runtime, fixture.secondListId);
    auto const viewB = runtime.workspace().snapshot().activeViewId;
    REQUIRE(runtime.workspace().closeView(viewB));

    CHECK(runtime.workspace().goBack());
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.firstListId);
  }

  TEST_CASE("WorkspaceService - navigation history signal skips deduplicated navigation",
            "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.firstListId);
    auto const before = runtime.workspace().snapshot();

    std::int32_t callCount = 0;
    auto const sub = runtime.workspace().onChanged([&](WorkspaceChanged const&) noexcept { ++callCount; });

    requireNavigation(runtime, fixture.firstListId);
    CHECK(runtime.workspace().snapshot() == before);
    CHECK(callCount == 0);
  }

  TEST_CASE("WorkspaceService - goBack and goForward do not grow history", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.firstListId);
    requireNavigation(runtime, fixture.secondListId);

    requireBackNavigation(runtime);
    requireForwardNavigation(runtime);
    requireBackNavigation(runtime);

    requireForwardNavigation(runtime);
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.secondListId);
    CHECK(runtime.workspace().canGoBack());
    CHECK_FALSE(runtime.workspace().canGoForward());

    requireBackNavigation(runtime);
    CHECK(runtime.views().trackListState(runtime.workspace().snapshot().activeViewId).listId == fixture.firstListId);
    auto const boundaryRes = runtime.workspace().goBack();
    REQUIRE_FALSE(boundaryRes);
    CHECK(boundaryRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("WorkspaceService - repeated back navigation returns to the source list",
            "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.firstListId);
    requireNavigation(runtime, fixture.secondListId);
    requireNavigation(runtime, fixture.thirdListId);
    requireBackNavigation(runtime);
    requireBackNavigation(runtime);

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.firstListId);
  }

  TEST_CASE("WorkspaceService - goBack recreates destroyed views", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const listA = fixture.createList("A");
    auto const listB = fixture.createList("B");

    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    auto const expectedPresentation = normalizeTrackPresentationSpec(albumsPreset->spec);
    requireNavigation(runtime,
                      NavigationRequest{
                        .target = FilteredListTarget{.listId = listA, .filterExpression = "$title ~ \"A\""},
                        .recordHistory = true,
                        .optPresentation =
                          NavigationPresentation{
                            .mode = NavigationPresentationMode::Override,
                            .spec = expectedPresentation,
                          },
                      });
    auto const viewA = runtime.workspace().snapshot().activeViewId;

    requireNavigation(runtime, NavigationRequest{.target = listB, .recordHistory = true});
    auto const viewB = runtime.workspace().snapshot().activeViewId;

    CHECK(viewA != viewB);

    REQUIRE(runtime.workspace().closeView(viewA));
    settleRuntimeCallbacks(runtime);
    auto const beforeReplay = runtime.workspace().snapshot();
    auto changes = std::vector<WorkspaceChanged>{};
    auto const sub =
      runtime.workspace().onChanged([&](WorkspaceChanged const& changed) noexcept { changes.push_back(changed); });

    REQUIRE(runtime.workspace().goBack());
    settleRuntimeCallbacks(runtime);

    auto const replayed = runtime.workspace().snapshot();
    auto const newViewA = replayed.activeViewId;
    CHECK(newViewA != kInvalidViewId);
    CHECK(newViewA != viewA);
    CHECK(replayed.openViews == std::vector<ViewId>{viewB, newViewA});
    CHECK(replayed.revision == beforeReplay.revision + 1);
    auto const replayedState = runtime.views().trackListState(newViewA);
    CHECK(replayedState.listId == listA);
    CHECK(replayedState.filterExpression == "$title ~ \"A\"");
    CHECK(replayedState.presentation == expectedPresentation);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].cause == WorkspaceChangeCause::Navigation);
    CHECK(changes[0].snapshot == replayed);
  }

  TEST_CASE("WorkspaceService - failed goBack leaves workspace state unchanged", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const viewA = requireNavigation(runtime, fixture.firstListId);
    auto const viewB = requireNavigation(runtime, fixture.secondListId);
    REQUIRE(runtime.workspace().closeView(viewA));
    REQUIRE(runRuntimeTask(runtime, runtime.library().commands().deleteListAsync(fixture.firstListId)));
    auto const before = runtime.workspace().snapshot();
    CHECK(runtime.workspace().canGoBack());
    CHECK_FALSE(runtime.workspace().canGoForward());

    auto const res = runtime.workspace().goBack();

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotFound);
    auto const after = runtime.workspace().snapshot();
    CHECK(after == before);
    CHECK(after.activeViewId == viewB);
    CHECK(runtime.workspace().canGoBack());
    CHECK_FALSE(runtime.workspace().canGoForward());

    requireNavigation(runtime, fixture.thirdListId);
    requireBackNavigation(runtime);
    CHECK(runtime.workspace().snapshot().activeViewId == viewB);
  }

  TEST_CASE("WorkspaceService - failed goForward leaves workspace state unchanged",
            "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const viewA = requireNavigation(runtime, fixture.firstListId);
    auto const viewB = requireNavigation(runtime, fixture.secondListId);
    requireBackNavigation(runtime);
    REQUIRE(runtime.workspace().closeView(viewB));
    REQUIRE(runRuntimeTask(runtime, runtime.library().commands().deleteListAsync(fixture.secondListId)));
    auto const before = runtime.workspace().snapshot();
    CHECK_FALSE(runtime.workspace().canGoBack());
    CHECK(runtime.workspace().canGoForward());

    auto const res = runtime.workspace().goForward();

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotFound);
    auto const after = runtime.workspace().snapshot();
    CHECK(after == before);
    CHECK(after.activeViewId == viewA);
    CHECK_FALSE(runtime.workspace().canGoBack());
    CHECK(runtime.workspace().canGoForward());

    requireNavigation(runtime, fixture.thirdListId);
    requireBackNavigation(runtime);
    CHECK(runtime.workspace().snapshot().activeViewId == viewA);
  }
} // namespace ao::rt::test
