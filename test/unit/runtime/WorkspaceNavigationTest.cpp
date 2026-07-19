// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/runtime/WorkspaceTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Exception.h>
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

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::test;

  TEST_CASE("WorkspaceService - first navigateTo opens the target list", "[runtime][unit][workspace][navigation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    REQUIRE(runtime.workspace().navigateTo(fixture.firstListId));

    CHECK(runtime.workspace().canGoBack() == false);
    CHECK(runtime.workspace().canGoForward() == false);
    auto const layout = runtime.workspace().snapshot();
    CHECK(layout.activeViewId != kInvalidViewId);
    auto const state = runtime.views().trackListState(layout.activeViewId);
    CHECK(state.listId == fixture.firstListId);
  }

  TEST_CASE("WorkspaceService - navigateTo AllTracks opens the global list", "[runtime][unit][workspace][navigation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    REQUIRE(runtime.workspace().navigateTo(fixture.firstListId));
    REQUIRE(runtime.workspace().navigateTo(GlobalViewKind::AllTracks));

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == kAllTracksListId);
    CHECK(runtime.workspace().canGoBack() == true);
  }

  TEST_CASE("WorkspaceService - filtered AllTracks navigation uses the global list",
            "[runtime][unit][workspace][navigation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    REQUIRE(runtime.workspace().navigateTo(
      FilteredListTarget{.listId = kAllTracksListId, .filterExpression = "$genre = \"Rock\""}));

    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == rt::kAllTracksListId);
    CHECK(state.filterExpression == "$genre = \"Rock\"");
  }

  TEST_CASE("AppRuntime - jumpToAlbum rejects invalid tracks", "[runtime][unit][workspace][navigation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    REQUIRE(runtime.workspace().navigateTo(fixture.firstListId));
    auto const result = runtime.jumpToAlbum(kInvalidTrackId);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidInput);
    CHECK(runtime.workspace().canGoBack() == false);
    auto const state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == fixture.firstListId);
  }

  TEST_CASE("WorkspaceService - onChanged includes the committed focus", "[runtime][unit][workspace][focus]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    auto focusedViewId = kInvalidViewId;
    auto const sub = runtime.workspace().onChanged([&](WorkspaceChanged const& changed)
                                                   { focusedViewId = changed.snapshot.activeViewId; });

    REQUIRE(runtime.workspace().navigateTo(fixture.firstListId));
    auto activeViewId = runtime.workspace().snapshot().activeViewId;
    CHECK(focusedViewId == activeViewId);
  }

  TEST_CASE("WorkspaceService - deleting a list closes its open views", "[runtime][unit][workspace][lifecycle]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    auto listId = ao::test::requireValue(runtime.library().writer().createList(
      LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "Test List"}));
    REQUIRE(runtime.workspace().navigateTo(listId));

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
    auto& runtime = fixture.runtime;
    requireNavigation(runtime, FilteredListTarget{.listId = fixture.firstListId, .filterExpression = "$title ~ \"A\""});
    requireNavigation(runtime, FilteredListTarget{.listId = fixture.firstListId, .filterExpression = "$title ~ \"B\""});
    auto const before = runtime.workspace().snapshot();
    auto changes = std::vector<WorkspaceChanged>{};
    auto const sub =
      runtime.workspace().onChanged([&](WorkspaceChanged const& changed) { changes.push_back(changed); });

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
    auto& runtime = fixture.runtime;

    auto const trackId =
      TrackId{100}; // jumpToAlbum doesn't validate if track exists in library, it just passes the ID to playback

    bool revealCalled = false;
    auto const sub = runtime.playback().events().onRevealTrackRequested(
      [&](PlaybackRevealTrackRequest const& req)
      {
        if (req.trackId == trackId)
        {
          revealCalled = true;
        }
      });

    auto const result = runtime.jumpToAlbum(trackId);
    REQUIRE(result);
    CHECK(revealCalled == true);
    CHECK(result->afterRevision == runtime.workspace().snapshot().revision);

    auto state = runtime.views().trackListState(runtime.workspace().snapshot().activeViewId);
    CHECK(state.listId == kAllTracksListId);
    CHECK(state.presentation.id == "albums");
  }

  TEST_CASE("WorkspaceService - invalid navigation targets return an error", "[runtime][unit][workspace][navigation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;

    auto const result = runtime.workspace().navigateTo(static_cast<GlobalViewKind>(999));

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidInput);
    CHECK(runtime.workspace().snapshot().activeViewId == kInvalidViewId);
  }

  TEST_CASE("WorkspaceService - missing-list navigation leaves views focus and history unchanged",
            "[runtime][unit][workspace][navigation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;
    REQUIRE(runtime.workspace().navigateTo(fixture.firstListId));
    auto const beforeLayout = runtime.workspace().snapshot();
    auto const beforeViews = runtime.views().listViews();

    auto const result = runtime.workspace().navigateTo(ListId{999999});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
    auto const afterLayout = runtime.workspace().snapshot();
    CHECK(afterLayout.activeViewId == beforeLayout.activeViewId);
    CHECK(afterLayout.openViews == beforeLayout.openViews);
    CHECK(afterLayout.revision == beforeLayout.revision);
    CHECK(runtime.views().listViews().size() == beforeViews.size());
    CHECK_FALSE(runtime.workspace().canGoBack());
    CHECK_FALSE(runtime.workspace().canGoForward());
  }

  TEST_CASE("WorkspaceService - focus rejects ids outside the open live aggregate",
            "[runtime][unit][workspace][validation]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& runtime = fixture.runtime;
    auto const activeViewId = requireNavigation(runtime, fixture.firstListId);
    auto const unopenedView =
      ao::test::requireValue(runtime.views().createView(TrackListViewConfig{.listId = fixture.secondListId}));
    auto const before = runtime.workspace().snapshot();

    for (auto const viewId : {kInvalidViewId, unopenedView.viewId, ViewId{999999}})
    {
      auto const result = runtime.workspace().focusView(viewId);
      REQUIRE_FALSE(result);
    }

    CHECK(runtime.workspace().snapshot() == before);
    CHECK(runtime.workspace().snapshot().activeViewId == activeViewId);
  }

  TEST_CASE("WorkspaceService - no-op focus and close preserve revision and observation",
            "[runtime][unit][workspace][receipt]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& workspace = fixture.runtime.workspace();
    auto const activeViewId = requireNavigation(fixture.runtime, fixture.firstListId);
    auto const before = workspace.snapshot();
    auto const unopened =
      ao::test::requireValue(fixture.runtime.views().createView(TrackListViewConfig{.listId = fixture.secondListId}));
    std::int32_t changeCount = 0;
    auto const sub = workspace.onChanged([&](WorkspaceChanged const&) { ++changeCount; });

    auto const focus = ao::test::requireValue(workspace.focusView(activeViewId));
    auto const close = ao::test::requireValue(workspace.closeView(unopened.viewId));

    CHECK(focus.disposition == WorkspaceCommitDisposition::NoChange);
    CHECK(close.disposition == WorkspaceCommitDisposition::NoChange);
    CHECK(focus.beforeRevision == before.revision);
    CHECK(focus.afterRevision == before.revision);
    CHECK(workspace.snapshot() == before);
    CHECK(fixture.runtime.views().trackListState(unopened.viewId).lifecycle == ViewLifecycleState::Attached);
    CHECK(changeCount == 0);
  }

  TEST_CASE("WorkspaceService - focus commits a different open live view once", "[runtime][unit][workspace][receipt]")
  {
    auto fixture = WorkspaceRuntimeFixture{};
    auto& workspace = fixture.runtime.workspace();
    auto const firstViewId = requireNavigation(fixture.runtime, fixture.firstListId);
    requireNavigation(fixture.runtime, fixture.secondListId);
    auto const before = workspace.snapshot();
    auto changed = WorkspaceChanged{};
    auto const sub = workspace.onChanged([&](WorkspaceChanged const& value) { changed = value; });

    auto const receipt = ao::test::requireValue(workspace.focusView(firstViewId));

    CHECK(receipt.disposition == WorkspaceCommitDisposition::Applied);
    CHECK(receipt.beforeRevision == before.revision);
    CHECK(receipt.afterRevision == before.revision + 1);
    CHECK(receipt.activeViewId == firstViewId);
    CHECK(changed.cause == WorkspaceChangeCause::Focus);
    CHECK(changed.snapshot == workspace.snapshot());
  }

  TEST_CASE("WorkspaceService - changed observations are deferred and contain observer failures",
            "[runtime][unit][workspace][observation]")
  {
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto runtime = makeRuntime(tempDir, std::move(executorPtr));
    auto received = std::vector<WorkspaceChanged>{};
    auto const throwingSub =
      runtime.workspace().onChanged([](WorkspaceChanged const&) { throwException<Exception>("observer failed"); });
    auto const receivingSub =
      runtime.workspace().onChanged([&](WorkspaceChanged const& changed) { received.push_back(changed); });

    auto const receipt = ao::test::requireValue(runtime.workspace().navigateTo(GlobalViewKind::AllTracks));

    CHECK(receipt.disposition == WorkspaceCommitDisposition::Applied);
    CHECK(received.empty());
    CHECK_NOTHROW(executor->drain());
    REQUIRE(received.size() == 1);
    CHECK(received.front().snapshot == runtime.workspace().snapshot());
    CHECK(received.front().snapshot.revision == receipt.afterRevision);
  }

  TEST_CASE("WorkspaceService - reentrant changes cannot mutate the observation being delivered",
            "[runtime][unit][workspace][observation]")
  {
    auto tempDir = ao::test::TempDir{};
    auto executorPtr = std::make_unique<QueuedExecutor>();
    auto* const executor = executorPtr.get();
    auto runtime = makeRuntime(tempDir, std::move(executorPtr));
    auto const firstListId = ao::test::requireValue(runtime.library().writer().createList(
      LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "First observed"}));
    executor->drain();
    auto const secondListId = ao::test::requireValue(runtime.library().writer().createList(
      LibraryWriter::ListDraft{.kind = LibraryWriter::ListKind::Manual, .name = "Second observed"}));
    executor->drain();
    auto received = std::vector<WorkspaceChanged>{};
    auto const sub = runtime.workspace().onChanged(
      [&](WorkspaceChanged const& changed)
      {
        received.push_back(changed);

        if (received.size() == 1)
        {
          REQUIRE(runtime.workspace().navigateTo(secondListId));
        }
      });

    auto const firstReceipt = ao::test::requireValue(runtime.workspace().navigateTo(firstListId));
    REQUIRE(executor->drainUntil([&] { return received.size() == 1; }));

    REQUIRE(received.size() == 1);
    auto const firstObservation = received.front();
    CHECK(firstObservation.snapshot.revision == firstReceipt.afterRevision);
    CHECK(runtime.workspace().snapshot().revision == firstReceipt.afterRevision + 1);
    CHECK(received.front() == firstObservation);

    REQUIRE(executor->drainUntil([&] { return received.size() == 2; }));
    CHECK(received.back().snapshot == runtime.workspace().snapshot());
  }
} // namespace ao::rt::test
