// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace ao::rt::test
{
  using namespace ao::test;

  TEST_CASE("WorkspaceService - second navigateTo enables back navigation", "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});

    CHECK(runtime.workspace().canGoBack() == true);
    CHECK(runtime.workspace().canGoForward() == false);
  }

  TEST_CASE("WorkspaceService - navigateTo deduplicates the current list", "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});
    runtime.workspace().navigateTo(ListId{20});

    CHECK(runtime.workspace().canGoBack() == true);
    runtime.workspace().goBack();
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{10});
  }

  TEST_CASE("WorkspaceService - navigateTo can skip recording history", "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20}, {.recordHistory = false});

    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{20});
    CHECK(runtime.workspace().canGoBack() == false);
  }

  TEST_CASE("WorkspaceService - navigateTo filtered target records filter history",
            "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(
      FilteredListTarget{.listId = kAllTracksListId, .filterExpression = "genre == \"Rock\""});

    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.filterExpression == "genre == \"Rock\"");
    CHECK(runtime.workspace().canGoBack() == true);

    runtime.workspace().goBack();
    auto const backState = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(backState.listId == ListId{10});
    CHECK(backState.filterExpression.empty());
  }

  TEST_CASE("WorkspaceService - goBack restores the previous list", "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});
    runtime.workspace().navigateTo(ListId{30});

    CHECK(runtime.workspace().goBack());
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{20});
    CHECK(runtime.workspace().canGoBack() == true);
    CHECK(runtime.workspace().canGoForward() == true);
  }

  TEST_CASE("WorkspaceService - repeated goBack restores the first list", "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});
    runtime.workspace().navigateTo(ListId{30});

    runtime.workspace().goBack();
    runtime.workspace().goBack();

    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{10});
    CHECK(runtime.workspace().canGoBack() == false);
    CHECK(runtime.workspace().canGoForward() == true);
  }

  TEST_CASE("WorkspaceService - goForward after back restores the newer list", "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});
    runtime.workspace().navigateTo(ListId{30});
    runtime.workspace().goBack();

    CHECK(runtime.workspace().goForward());
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{30});
    CHECK(runtime.workspace().canGoForward() == false);
  }

  TEST_CASE("WorkspaceService - goBack at the first entry returns false", "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    CHECK_FALSE(runtime.workspace().goBack());
  }

  TEST_CASE("WorkspaceService - goForward at the newest entry returns false", "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    CHECK_FALSE(runtime.workspace().goForward());
  }

  TEST_CASE("WorkspaceService - new navigation after back truncates forward history",
            "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});
    runtime.workspace().navigateTo(ListId{30});
    runtime.workspace().goBack();
    runtime.workspace().navigateTo(ListId{40});

    CHECK(runtime.workspace().canGoForward() == false);
    runtime.workspace().goBack();
    auto const midState = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(midState.listId == ListId{20});
    runtime.workspace().goBack();
    auto const firstState = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(firstState.listId == ListId{10});
  }

  TEST_CASE("WorkspaceService - goBack restores presentation state", "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    runtime.workspace().setActivePresentation(albumsPreset->spec);

    runtime.workspace().goBack();
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.presentation.id == "songs");
  }

  TEST_CASE("WorkspaceService - goBack works after closing the active view", "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});
    auto const viewB = runtime.workspace().layoutState().activeViewId;
    runtime.workspace().closeView(viewB);

    CHECK(runtime.workspace().goBack());
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{10});
  }

  TEST_CASE("WorkspaceService - navigation history signal emits on navigate", "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto received = WorkspaceService::NavigationHistoryChanged{};
    auto const sub = runtime.workspace().onNavigationHistoryChanged([&](auto const& ev) { received = ev; });

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});

    CHECK(received.canGoBack == true);
    CHECK(received.canGoForward == false);
  }

  TEST_CASE("WorkspaceService - navigation history signal emits on back", "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});

    auto received = WorkspaceService::NavigationHistoryChanged{};
    auto const sub = runtime.workspace().onNavigationHistoryChanged([&](auto const& ev) { received = ev; });

    runtime.workspace().goBack();

    CHECK(received.canGoBack == false);
    CHECK(received.canGoForward == true);
  }

  TEST_CASE("WorkspaceService - navigation history signal skips deduplicated navigation",
            "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});

    std::int32_t callCount = 0;
    auto const sub = runtime.workspace().onNavigationHistoryChanged([&](auto const&) { ++callCount; });

    runtime.workspace().navigateTo(ListId{10});
    CHECK(callCount == 0);
  }

  TEST_CASE("WorkspaceService - goBack and goForward do not grow history", "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});

    runtime.workspace().goBack();
    runtime.workspace().goForward();
    runtime.workspace().goBack();

    runtime.workspace().goForward();
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{20});
  }

  TEST_CASE("WorkspaceService - repeated back navigation returns to the source list",
            "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(ListId{20});
    runtime.workspace().navigateTo(ListId{30});
    runtime.workspace().goBack();
    runtime.workspace().goBack();

    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{10});
    CHECK(runtime.workspace().canGoBack() == false);
    CHECK(runtime.workspace().canGoForward() == true);
  }

  TEST_CASE("WorkspaceService - goBack recreates destroyed views", "[runtime][unit][workspace][history]")
  {
    auto tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto const listA =
      ao::test::requireValue(runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "A"}));
    auto const listB =
      ao::test::requireValue(runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "B"}));

    runtime.workspace().navigateTo(listA, {.recordHistory = true});
    auto const viewA = runtime.workspace().layoutState().activeViewId;

    runtime.workspace().navigateTo(listB, {.recordHistory = true});
    auto const viewB = runtime.workspace().layoutState().activeViewId;

    CHECK(viewA != viewB);

    runtime.workspace().closeView(viewA);

    CHECK(runtime.workspace().goBack());

    auto const newViewA = runtime.workspace().layoutState().activeViewId;
    CHECK(newViewA != kInvalidViewId);
    CHECK(newViewA != viewA);
  }
} // namespace ao::rt::test
