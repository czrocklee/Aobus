// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/runtime/AppRuntimeTestSupport.h"
#include "test/runtime/WorkspaceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/LibraryCommands.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::test;

  TEST_CASE("WorkspaceService - navigate deduplicates the current list")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);
    requireNavigation(runtime, fixture.listIds().second);
    requireNavigation(runtime, fixture.listIds().second);

    requireBackNavigation(runtime);
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.listIds().first);
  }

  TEST_CASE("WorkspaceService - navigate request can skip recording history")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);
    requireNavigation(runtime, NavigationRequest{.target = fixture.listIds().second, .recordHistory = false});

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.listIds().second);
    CHECK_FALSE(runtime.workspace().goBack());
  }

  TEST_CASE("WorkspaceService - navigate filtered target records filter history")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);
    requireNavigation(runtime, FilteredListTarget{.listId = kAllTracksListId, .filterExpression = "genre == \"Rock\""});

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.filterExpression == "genre == \"Rock\"");

    requireBackNavigation(runtime);
    auto const backState = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(backState.listId == fixture.listIds().first);
    CHECK(backState.filterExpression.empty());
  }

  TEST_CASE("WorkspaceService - plain navigation to the same list records the quick-filtered departure")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& views = runtime.views();
    auto const filtered = requireNavigation(runtime, fixture.listIds().first);
    auto const* const filter = R"($title = "Before")";
    REQUIRE(views.setFilter(filtered, filter));
    settleRuntimeCallbacks(runtime);
    CHECK_FALSE(runtime.workspace().canGoBack());

    auto const plain = requireNavigation(runtime, fixture.listIds().first);

    CHECK(plain != filtered);
    auto const plainState = views.trackListState(plain);
    CHECK(plainState.listId == fixture.listIds().first);
    CHECK(plainState.filterExpression.empty());
    REQUIRE(requireBackNavigation(runtime) == filtered);
    CHECK(views.trackListState(filtered).filterExpression == filter);
    CHECK(requireForwardNavigation(runtime) == plain);
  }

  TEST_CASE("WorkspaceService - goBack restores the previous list")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);
    CHECK_FALSE(runtime.workspace().canGoBack());
    CHECK_FALSE(runtime.workspace().canGoForward());
    requireNavigation(runtime, fixture.listIds().second);
    CHECK(runtime.workspace().canGoBack());
    CHECK_FALSE(runtime.workspace().canGoForward());
    requireNavigation(runtime, fixture.listIds().third);

    CHECK(runtime.workspace().goBack());
    CHECK(runtime.workspace().canGoBack());
    CHECK(runtime.workspace().canGoForward());
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.listIds().second);
  }

  TEST_CASE("WorkspaceService - repeated goBack restores the first list")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);
    requireNavigation(runtime, fixture.listIds().second);
    requireNavigation(runtime, fixture.listIds().third);

    requireBackNavigation(runtime);
    requireBackNavigation(runtime);

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.listIds().first);
    CHECK_FALSE(runtime.workspace().goBack());
  }

  TEST_CASE("WorkspaceService - goForward after back restores the newer list")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);
    requireNavigation(runtime, fixture.listIds().second);
    requireNavigation(runtime, fixture.listIds().third);
    requireBackNavigation(runtime);

    CHECK(runtime.workspace().goForward());
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.listIds().third);
    CHECK_FALSE(runtime.workspace().goForward());
  }

  TEST_CASE("WorkspaceService - goBack at the first entry reports NotFound without changing the workspace")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);
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

  TEST_CASE("WorkspaceService - goForward at the newest entry reports NotFound without changing the workspace")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);
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

  TEST_CASE("WorkspaceService - new navigation after back truncates forward history")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);
    requireNavigation(runtime, fixture.listIds().second);
    requireNavigation(runtime, fixture.listIds().third);
    requireBackNavigation(runtime);
    requireNavigation(runtime, fixture.listIds().fourth);

    CHECK_FALSE(runtime.workspace().goForward());
    requireBackNavigation(runtime);
    auto const midState = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(midState.listId == fixture.listIds().second);
    requireBackNavigation(runtime);
    auto const firstState = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(firstState.listId == fixture.listIds().first);
  }

  TEST_CASE("WorkspaceService - goBack restores presentation state")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);
    REQUIRE(runtime.workspace().setActivePresentation(builtinTrackPresentationPreset(BuiltinPreset::Albums).spec));

    requireBackNavigation(runtime);
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.presentation == normalizeTrackPresentationSpec(defaultTrackPresentationSpec()));
  }

  TEST_CASE("WorkspaceService - Back and Forward restore each navigation point's column sort")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& workspace = runtime.workspace();
    auto& views = runtime.views();
    auto expected = builtinTrackPresentationPreset(BuiltinPreset::Songs).spec;
    auto const first = requireNavigation(runtime,
                                         NavigationRequest{
                                           .target = fixture.listIds().first,
                                           .optPresentation = NavigationPresentation{.spec = expected},
                                         });
    auto const displayNumber = TrackColumnSort{TrackField::DisplayTrackNumber, false};
    auto const trackNumber = TrackColumnSort{TrackField::TrackNumber, false};
    auto const album = TrackColumnSort{TrackField::Album, false};
    REQUIRE(views.setColumnSort(first, displayNumber));
    expected.optColumnSort = displayNumber;
    auto const second = requireNavigation(runtime, fixture.listIds().second);
    // The inactive view now has the same comparison keys but a different clicked column.
    REQUIRE(views.setColumnSort(first, trackNumber));
    REQUIRE(views.setColumnSort(second, album));
    auto const secondPresentation = views.trackListState(second).presentation;

    REQUIRE(requireBackNavigation(runtime) == first);
    CHECK(views.trackListState(first).presentation == expected);
    CHECK_FALSE(workspace.canGoBack());
    REQUIRE(workspace.canGoForward());
    REQUIRE(views.setColumnSort(first, trackNumber));
    expected.optColumnSort = trackNumber;
    REQUIRE(views.setColumnSort(second, displayNumber));

    REQUIRE(requireForwardNavigation(runtime) == second);
    CHECK(views.trackListState(second).presentation == secondPresentation);
    CHECK_FALSE(workspace.canGoForward());
    REQUIRE(requireBackNavigation(runtime) == first);
    CHECK(views.trackListState(first).presentation == expected);
    CHECK_FALSE(workspace.canGoBack());
  }

  TEST_CASE("WorkspaceService - Forward restores a different presentation after sorting a matching filtered view")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& workspace = runtime.workspace();
    auto& views = runtime.views();
    auto const target = FilteredListTarget{.listId = fixture.listIds().first, .filterExpression = R"($title ~ "A")"};
    auto const songs = builtinTrackPresentationPreset(BuiltinPreset::Songs).spec;
    auto forwardPresentation = builtinTrackPresentationPreset(BuiltinPreset::Albums).spec;

    SECTION("The presentations use different presets")
    {
    }

    SECTION("The same preset id has different visible fields")
    {
      forwardPresentation = songs;
      forwardPresentation.visibleFields = {TrackField::Title};
    }

    auto const first = requireNavigation(
      runtime, NavigationRequest{.target = target, .optPresentation = NavigationPresentation{.spec = songs}});
    auto const second = requireNavigation(
      runtime,
      NavigationRequest{.target = target, .optPresentation = NavigationPresentation{.spec = forwardPresentation}});
    REQUIRE(first != second);
    REQUIRE(workspace.focusView(first));
    REQUIRE(views.setColumnSort(first, {TrackField::Duration, false}));

    REQUIRE(requireBackNavigation(runtime) == first);
    CHECK(views.trackListState(first).presentation == songs);
    // Remove both live matches so Forward must reconstruct the recorded point.
    REQUIRE(workspace.closeView(first));
    REQUIRE(workspace.closeView(second));

    auto const recovered = views.trackListState(requireForwardNavigation(runtime));
    CHECK(recovered.listId == target.listId);
    CHECK(recovered.filterExpression == target.filterExpression);
    CHECK(recovered.presentation == forwardPresentation);
  }

  TEST_CASE("WorkspaceService - repeating the active view after a column sort preserves Forward")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& workspace = runtime.workspace();
    auto& views = runtime.views();
    auto const songs = builtinTrackPresentationPreset(BuiltinPreset::Songs).spec;
    auto const first = requireNavigation(
      runtime,
      NavigationRequest{.target = fixture.listIds().first, .optPresentation = NavigationPresentation{.spec = songs}});
    auto const second = requireNavigation(runtime, fixture.listIds().second);
    REQUIRE(requireBackNavigation(runtime) == first);
    auto const duration = TrackColumnSort{TrackField::Duration, false};
    REQUIRE(views.setColumnSort(first, duration));
    settleRuntimeCallbacks(runtime);
    auto const sorted = views.trackListState(first).presentation;
    REQUIRE(sorted.optColumnSort == duration);
    auto const before = workspace.snapshot();
    auto changes = std::vector<WorkspaceChanged>{};
    auto const sub = workspace.onChanged([&](WorkspaceChanged const& changed) noexcept { changes.push_back(changed); });
    auto request = NavigationRequest{.target = fixture.listIds().first};

    SECTION("A repeat without a presentation")
    {
    }

    SECTION("A repeat with a new-view default")
    {
      request.optPresentation = NavigationPresentation{
        .mode = NavigationPresentationMode::NewViewDefault,
        .spec = builtinTrackPresentationPreset(BuiltinPreset::Albums).spec,
      };
    }

    SECTION("A repeat with an equivalent override")
    {
      request.optPresentation = NavigationPresentation{.spec = sorted};
    }

    REQUIRE(requireNavigation(runtime, request) == first);

    CHECK(workspace.snapshot() == before);
    CHECK(changes.empty());
    CHECK(views.trackListState(first).presentation == sorted);
    CHECK_FALSE(workspace.canGoBack());
    REQUIRE(workspace.canGoForward());
    CHECK(requireForwardNavigation(runtime) == second);
    CHECK_FALSE(workspace.canGoForward());
    REQUIRE(requireBackNavigation(runtime) == first);
    CHECK(views.trackListState(first).presentation == sorted);
    CHECK_FALSE(workspace.canGoBack());
  }

  TEST_CASE("WorkspaceService - goBack works after closing the active view")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);
    requireNavigation(runtime, fixture.listIds().second);
    auto const viewB = runtime.workspace().snapshot().activeViewId;
    REQUIRE(runtime.workspace().closeView(viewB));

    CHECK(runtime.workspace().goBack());
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.listIds().first);
  }

  TEST_CASE("WorkspaceService - navigation history signal skips deduplicated navigation")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);
    auto const before = runtime.workspace().snapshot();

    std::int32_t callCount = 0;
    auto const sub = runtime.workspace().onChanged([&](WorkspaceChanged const&) noexcept { ++callCount; });

    requireNavigation(runtime, fixture.listIds().first);
    CHECK(runtime.workspace().snapshot() == before);
    CHECK(callCount == 0);
  }

  TEST_CASE("WorkspaceService - goBack and goForward do not grow history")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);
    requireNavigation(runtime, fixture.listIds().second);

    requireBackNavigation(runtime);
    requireForwardNavigation(runtime);
    requireBackNavigation(runtime);

    requireForwardNavigation(runtime);
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.listIds().second);
    CHECK(runtime.workspace().canGoBack());
    CHECK_FALSE(runtime.workspace().canGoForward());

    requireBackNavigation(runtime);
    CHECK(runtime.views().trackListState(runtime.workspace().snapshot().activeViewId).listId ==
          fixture.listIds().first);
    auto const boundaryRes = runtime.workspace().goBack();
    REQUIRE_FALSE(boundaryRes);
    CHECK(boundaryRes.error().code == Error::Code::NotFound);
  }

  TEST_CASE("WorkspaceService - repeated back navigation returns to the source list")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    requireNavigation(runtime, fixture.listIds().first);
    requireNavigation(runtime, fixture.listIds().second);
    requireNavigation(runtime, fixture.listIds().third);
    requireBackNavigation(runtime);
    requireBackNavigation(runtime);

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.listIds().first);
  }

  TEST_CASE("WorkspaceService - goBack recreates destroyed views")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const listA = fixture.createList("A");
    auto const listB = fixture.createList("B");

    auto const expectedPresentation =
      normalizeTrackPresentationSpec(builtinTrackPresentationPreset(BuiltinPreset::Albums).spec);
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

  TEST_CASE("WorkspaceService - failed goBack leaves workspace state unchanged")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const viewA = requireNavigation(runtime, fixture.listIds().first);
    auto const viewB = requireNavigation(runtime, fixture.listIds().second);
    REQUIRE(runtime.workspace().closeView(viewA));
    REQUIRE(runRuntimeTask(runtime, fixture.commands().deleteListAsync(fixture.listIds().first)));
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

    requireNavigation(runtime, fixture.listIds().third);
    requireBackNavigation(runtime);
    CHECK(runtime.workspace().snapshot().activeViewId == viewB);
  }

  TEST_CASE("WorkspaceService - failed goForward leaves workspace state unchanged")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const viewA = requireNavigation(runtime, fixture.listIds().first);
    auto const viewB = requireNavigation(runtime, fixture.listIds().second);
    requireBackNavigation(runtime);
    REQUIRE(runtime.workspace().closeView(viewB));
    REQUIRE(runRuntimeTask(runtime, fixture.commands().deleteListAsync(fixture.listIds().second)));
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

    requireNavigation(runtime, fixture.listIds().third);
    requireBackNavigation(runtime);
    CHECK(runtime.workspace().snapshot().activeViewId == viewA);
  }
} // namespace ao::rt::test
