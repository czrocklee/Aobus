// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/WorkspaceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/async/Signal.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/playback/PlaybackEvents.h>
#include <ao/rt/playback/PlaybackService.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::test;

  TEST_CASE("WorkspaceService - first navigate opens the target list", "[runtime][unit][workspace][navigation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    REQUIRE(runtime.workspace().navigate({.target = fixture.firstListId}));

    auto const layout = runtime.workspace().snapshot();
    CHECK(layout.activeViewId != kInvalidViewId);
    auto const state = runtime.views().trackListState(layout.activeViewId);
    CHECK(state.listId == fixture.firstListId);
  }

  TEST_CASE("WorkspaceService - navigate AllTracks opens the global list", "[runtime][unit][workspace][navigation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    REQUIRE(runtime.workspace().navigate({.target = fixture.firstListId}));
    REQUIRE(runtime.workspace().navigate({.target = GlobalViewKind::AllTracks}));

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == kAllTracksListId);
  }

  TEST_CASE("WorkspaceService - filtered AllTracks navigation uses the global list",
            "[runtime][unit][workspace][navigation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    REQUIRE(runtime.workspace().navigate({
      .target =
        FilteredListTarget{
          .listId = kAllTracksListId,
          .filterExpression = "$genre = \"Rock\"",
        },
    }));

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == rt::kAllTracksListId);
    CHECK(state.filterExpression == "$genre = \"Rock\"");
  }

  TEST_CASE("AppRuntime - jumpToAlbum rejects invalid tracks", "[runtime][unit][workspace][navigation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    REQUIRE(runtime.workspace().navigate({.target = fixture.firstListId}));
    auto const result = runtime.jumpToAlbum(kInvalidTrackId);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidInput);
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.firstListId);
  }

  TEST_CASE("WorkspaceService - onChanged includes the committed focus", "[runtime][unit][workspace][focus]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto focusedViewId = kInvalidViewId;
    auto const sub = runtime.workspace().onChanged([&](WorkspaceChanged const& changed) noexcept
                                                   { focusedViewId = changed.snapshot.activeViewId; });

    REQUIRE(runtime.workspace().navigate({.target = fixture.firstListId}));
    auto activeViewId = runtime.workspace().snapshot().activeViewId;
    CHECK(focusedViewId == activeViewId);
  }

  TEST_CASE("WorkspaceService - deleting a list closes its open views", "[runtime][unit][workspace][lifecycle]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto listId =
      ao::test::requireValue(runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "Test List"}));
    REQUIRE(runtime.workspace().navigate({.target = listId}));

    auto activeViewId = runtime.workspace().snapshot().activeViewId;
    CHECK(activeViewId != kInvalidViewId);

    REQUIRE(runtime.library().writer().deleteList(listId));

    auto layout = runtime.workspace().snapshot();
    CHECK(!std::ranges::contains(layout.openViews, activeViewId));
  }

  TEST_CASE("WorkspaceService - deleting a list closes all matching views in one commit",
            "[runtime][unit][workspace][lifecycle]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    requireNavigation(runtime, FilteredListTarget{.listId = fixture.firstListId, .filterExpression = "$title ~ \"A\""});
    requireNavigation(runtime, FilteredListTarget{.listId = fixture.firstListId, .filterExpression = "$title ~ \"B\""});
    auto const before = runtime.workspace().snapshot();
    auto changes = std::vector<WorkspaceChanged>{};
    auto const sub =
      runtime.workspace().onChanged([&](WorkspaceChanged const& changed) noexcept { changes.push_back(changed); });

    REQUIRE(runtime.library().writer().deleteList(fixture.firstListId));

    REQUIRE(changes.size() == 1);
    CHECK(changes.front().cause == WorkspaceChangeCause::ListDeletion);
    CHECK(changes.front().snapshot.openViews.empty());
    CHECK(changes.front().snapshot.activeViewId == kInvalidViewId);
    CHECK(changes.front().snapshot.revision == before.revision + 1);
  }

  TEST_CASE("AppRuntime - jumpToAlbum reveals valid tracks in album presentation",
            "[runtime][unit][workspace][navigation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const trackId =
      TrackId{100}; // jumpToAlbum doesn't validate if track exists in library, it just passes the ID to playback
    auto const* songsPreset = builtinTrackPresentationPreset("songs");
    REQUIRE(songsPreset != nullptr);
    auto const existingViewId = ao::test::requireValue(runtime.workspace().navigate({
      .target = GlobalViewKind::AllTracks,
      .optPresentation =
        NavigationPresentation{
          .mode = NavigationPresentationMode::Override,
          .spec = songsPreset->spec,
        },
    }));

    bool revealCalled = false;
    auto const sub = runtime.playback().events().onRevealTrackRequested(
      [&](PlaybackRevealTrackRequest const& req) noexcept
      {
        if (req.trackId == trackId)
        {
          revealCalled = true;
        }
      });

    auto const result = runtime.jumpToAlbum(trackId);
    REQUIRE(result);
    CHECK(revealCalled == true);

    auto state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(runtime.workspace().snapshot().activeViewId == existingViewId);
    CHECK(state.listId == kAllTracksListId);
    CHECK(state.presentation.id == "albums");
  }

  TEST_CASE("WorkspaceService - invalid navigation targets return an error", "[runtime][unit][workspace][navigation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const result = runtime.workspace().navigate({.target = static_cast<GlobalViewKind>(999)});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidInput);
    CHECK(runtime.workspace().snapshot().activeViewId == kInvalidViewId);
  }

  TEST_CASE("WorkspaceService - empty navigation request rejects its invalid default target",
            "[runtime][unit][workspace][navigation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();

    auto const result = runtime.workspace().navigate({});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidInput);
    CHECK(runtime.workspace().snapshot().activeViewId == kInvalidViewId);
    CHECK(runtime.workspace().snapshot().openViews.empty());
  }

  TEST_CASE("WorkspaceService - missing-list navigation leaves views focus and history unchanged",
            "[runtime][unit][workspace][navigation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    REQUIRE(runtime.workspace().navigate({.target = fixture.firstListId}));
    auto const beforeLayout = runtime.workspace().snapshot();

    auto const result = runtime.workspace().navigate({.target = ListId{999999}});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
    auto const afterLayout = runtime.workspace().snapshot();
    CHECK(afterLayout.activeViewId == beforeLayout.activeViewId);
    CHECK(afterLayout.openViews == beforeLayout.openViews);
    CHECK(afterLayout.revision == beforeLayout.revision);
    CHECK(afterLayout.openViews.size() == beforeLayout.openViews.size());
  }

  TEST_CASE("WorkspaceService - focus rejects ids outside the open live aggregate",
            "[runtime][unit][workspace][validation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const activeViewId = requireNavigation(runtime, fixture.firstListId);
    auto const before = runtime.workspace().snapshot();

    for (auto const viewId : {kInvalidViewId, ViewId{999999}})
    {
      auto const result = runtime.workspace().focusView(viewId);
      REQUIRE_FALSE(result);
    }

    CHECK(runtime.workspace().snapshot() == before);
    CHECK(runtime.workspace().snapshot().activeViewId == activeViewId);
  }

  TEST_CASE("WorkspaceService - no-op focus and close preserve revision and observation",
            "[runtime][unit][workspace][observation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& workspace = fixture.runtime().workspace();
    auto const activeViewId = requireNavigation(fixture.runtime(), fixture.firstListId);
    auto const before = workspace.snapshot();
    std::int32_t changeCount = 0;
    auto const sub = workspace.onChanged([&](WorkspaceChanged const&) noexcept { ++changeCount; });

    REQUIRE(workspace.focusView(activeViewId));
    REQUIRE(workspace.closeView(ViewId{999999}));

    CHECK(workspace.snapshot() == before);
    CHECK(changeCount == 0);
  }

  TEST_CASE("WorkspaceService - focus commits a different open live view once",
            "[runtime][unit][workspace][observation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& workspace = fixture.runtime().workspace();
    auto const firstViewId = requireNavigation(fixture.runtime(), fixture.firstListId);
    requireNavigation(fixture.runtime(), fixture.secondListId);
    auto const before = workspace.snapshot();
    auto changed = WorkspaceChanged{};
    auto const sub = workspace.onChanged([&](WorkspaceChanged const& value) noexcept { changed = value; });

    REQUIRE(workspace.focusView(firstViewId));

    CHECK(workspace.snapshot().revision == before.revision + 1);
    CHECK(workspace.snapshot().activeViewId == firstViewId);
    CHECK(changed.cause == WorkspaceChangeCause::Focus);
    CHECK(changed.snapshot == workspace.snapshot());
  }

  TEST_CASE("WorkspaceService - changed observations are deferred to contract-fulfilling observers",
            "[runtime][unit][workspace][observation]")
  {
    // The handler type enforces the noexcept contract. These two
    // contract-fulfilling observers therefore receive the same committed
    // snapshot in connection order.
    STATIC_REQUIRE(
      std::is_nothrow_invocable_v<async::Signal<WorkspaceChanged const&>::Handler, WorkspaceChanged const&>);
    STATIC_REQUIRE_FALSE(std::is_constructible_v<async::Signal<WorkspaceChanged const&>::Handler,
                                                 decltype([](WorkspaceChanged const&) {})>);

    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = makeRuntime(tempDir, std::move(executorPtr));
    auto received = std::vector<WorkspaceChanged>{};
    bool leadingObserverEntered = false;
    auto const leadingSub =
      runtimePtr->workspace().onChanged([&](WorkspaceChanged const&) noexcept { leadingObserverEntered = true; });
    auto const receivingSub =
      runtimePtr->workspace().onChanged([&](WorkspaceChanged const& changed) noexcept { received.push_back(changed); });

    REQUIRE(runtimePtr->workspace().navigate({.target = GlobalViewKind::AllTracks}));

    CHECK(received.empty());
    CHECK_NOTHROW(executor->drain());
    CHECK(leadingObserverEntered);
    REQUIRE(received.size() == 1);
    CHECK(received.front().snapshot == runtimePtr->workspace().snapshot());
  }

  TEST_CASE("WorkspaceService - reentrant changes cannot mutate the observation being delivered",
            "[runtime][unit][workspace][observation]")
  {
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto runtimePtr = makeRuntime(tempDir, std::move(executorPtr));
    auto const firstListId = ao::test::requireValue(
      runtimePtr->library().writer().createList(LibraryWriter::ListDraft{.name = "First observed"}));
    executor->drain();
    auto const secondListId = ao::test::requireValue(
      runtimePtr->library().writer().createList(LibraryWriter::ListDraft{.name = "Second observed"}));
    executor->drain();
    auto received = std::vector<WorkspaceChanged>{};
    bool reentrantNavigateSucceeded = false;
    auto const sub = runtimePtr->workspace().onChanged(
      [&](WorkspaceChanged const& changed) noexcept
      {
        received.push_back(changed);

        if (received.size() == 1)
        {
          reentrantNavigateSucceeded = static_cast<bool>(runtimePtr->workspace().navigate({.target = secondListId}));
        }
      });

    REQUIRE(runtimePtr->workspace().navigate({.target = firstListId}));
    auto const firstRevision = runtimePtr->workspace().snapshot().revision;
    REQUIRE(executor->drainUntil([&] { return received.size() == 1; }));

    REQUIRE(received.size() == 1);
    CHECK(reentrantNavigateSucceeded);
    auto const firstObservation = received.front();
    CHECK(firstObservation.snapshot.revision == firstRevision);
    CHECK(runtimePtr->workspace().snapshot().revision == firstRevision + 1);
    CHECK(received.front() == firstObservation);

    REQUIRE(executor->drainUntil([&] { return received.size() == 2; }));
    CHECK(received.back().snapshot == runtimePtr->workspace().snapshot());
  }
} // namespace ao::rt::test
