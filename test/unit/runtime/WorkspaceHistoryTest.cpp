// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/runtime/WorkspaceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace ao::rt::test
{
  using namespace ao::test;

  TEST_CASE("WorkspaceService - navigate deduplicates the current list", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

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
    auto& runtime = fixture.runtime;

    requireNavigation(runtime, fixture.firstListId);
    requireNavigation(runtime, NavigationRequest{.target = fixture.secondListId, .recordHistory = false});

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.secondListId);
    CHECK_FALSE(runtime.workspace().goBack());
  }

  TEST_CASE("WorkspaceService - navigate filtered target records filter history", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

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
    auto& runtime = fixture.runtime;

    requireNavigation(runtime, fixture.firstListId);
    requireNavigation(runtime, fixture.secondListId);
    requireNavigation(runtime, fixture.thirdListId);

    CHECK(runtime.workspace().goBack());
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.secondListId);
  }

  TEST_CASE("WorkspaceService - repeated goBack restores the first list", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

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
    auto& runtime = fixture.runtime;

    requireNavigation(runtime, fixture.firstListId);
    requireNavigation(runtime, fixture.secondListId);
    requireNavigation(runtime, fixture.thirdListId);
    requireBackNavigation(runtime);

    CHECK(runtime.workspace().goForward());
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.thirdListId);
    CHECK_FALSE(runtime.workspace().goForward());
  }

  TEST_CASE("WorkspaceService - goBack at the first entry returns false", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    requireNavigation(runtime, fixture.firstListId);
    CHECK_FALSE(runtime.workspace().goBack());
  }

  TEST_CASE("WorkspaceService - goForward at the newest entry returns false", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    requireNavigation(runtime, fixture.firstListId);
    CHECK_FALSE(runtime.workspace().goForward());
  }

  TEST_CASE("WorkspaceService - new navigation after back truncates forward history",
            "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

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
    auto& runtime = fixture.runtime;

    requireNavigation(runtime, fixture.firstListId);
    auto const* albumsPreset = builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    REQUIRE(runtime.workspace().setActivePresentation(albumsPreset->spec));

    requireBackNavigation(runtime);
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.presentation.id == "list-order");
  }

  TEST_CASE("WorkspaceService - goBack works after closing the active view", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

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
    auto& runtime = fixture.runtime;

    requireNavigation(runtime, fixture.firstListId);

    std::int32_t callCount = 0;
    auto const sub = runtime.workspace().onChanged([&](WorkspaceChanged const&) { ++callCount; });

    requireNavigation(runtime, fixture.firstListId);
    CHECK(callCount == 0);
  }

  TEST_CASE("WorkspaceService - goBack and goForward do not grow history", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    requireNavigation(runtime, fixture.firstListId);
    requireNavigation(runtime, fixture.secondListId);

    requireBackNavigation(runtime);
    requireForwardNavigation(runtime);
    requireBackNavigation(runtime);

    requireForwardNavigation(runtime);
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.secondListId);
  }

  TEST_CASE("WorkspaceService - repeated back navigation returns to the source list",
            "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

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
    auto& runtime = fixture.runtime;

    auto const listA = ao::test::requireValue(runtime.library().writer().createList(
      LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "A"}));
    auto const listB = ao::test::requireValue(runtime.library().writer().createList(
      LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "B"}));

    requireNavigation(runtime, NavigationRequest{.target = listA, .recordHistory = true});
    auto const viewA = runtime.workspace().snapshot().activeViewId;

    requireNavigation(runtime, NavigationRequest{.target = listB, .recordHistory = true});
    auto const viewB = runtime.workspace().snapshot().activeViewId;

    CHECK(viewA != viewB);

    REQUIRE(runtime.workspace().closeView(viewA));

    CHECK(runtime.workspace().goBack());

    auto const newViewA = runtime.workspace().snapshot().activeViewId;
    CHECK(newViewA != kInvalidViewId);
    CHECK(newViewA != viewA);
  }

  TEST_CASE("WorkspaceService - failed goBack leaves workspace state unchanged", "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;
    auto const viewA = requireNavigation(runtime, fixture.firstListId);
    auto const viewB = requireNavigation(runtime, fixture.secondListId);
    REQUIRE(runtime.workspace().closeView(viewA));
    REQUIRE(runtime.library().writer().deleteList(fixture.firstListId));
    auto const before = runtime.workspace().snapshot();

    auto const result = runtime.workspace().goBack();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
    auto const after = runtime.workspace().snapshot();
    CHECK(after.activeViewId == viewB);
    CHECK(after.activeViewId == before.activeViewId);
    CHECK(after.openViews == before.openViews);
    CHECK(after.revision == before.revision);
  }

  TEST_CASE("WorkspaceService - failed goForward leaves workspace state unchanged",
            "[runtime][unit][workspace][history]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;
    auto const viewA = requireNavigation(runtime, fixture.firstListId);
    auto const viewB = requireNavigation(runtime, fixture.secondListId);
    requireBackNavigation(runtime);
    REQUIRE(runtime.workspace().closeView(viewB));
    REQUIRE(runtime.library().writer().deleteList(fixture.secondListId));
    auto const before = runtime.workspace().snapshot();

    auto const result = runtime.workspace().goForward();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
    auto const after = runtime.workspace().snapshot();
    CHECK(after.activeViewId == viewA);
    CHECK(after.activeViewId == before.activeViewId);
    CHECK(after.openViews == before.openViews);
    CHECK(after.revision == before.revision);
  }
} // namespace ao::rt::test
