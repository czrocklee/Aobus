// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <functional>

namespace ao::rt::test
{
  using namespace ao::test;

  TEST_CASE("WorkspaceService - first navigateTo opens the target list", "[runtime][unit][workspace][navigation]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});

    CHECK(runtime.workspace().canGoBack() == false);
    CHECK(runtime.workspace().canGoForward() == false);
    auto const layout = runtime.workspace().layoutState();
    CHECK(layout.activeViewId != kInvalidViewId);
    auto const state = runtime.views().trackListState(layout.activeViewId);
    CHECK(state.listId == ListId{10});
  }

  TEST_CASE("WorkspaceService - navigateTo AllTracks opens the global list", "[runtime][unit][workspace][navigation]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().navigateTo(GlobalViewKind::AllTracks);

    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == kAllTracksListId);
    CHECK(runtime.workspace().canGoBack() == true);
  }

  TEST_CASE("WorkspaceService - filtered AllTracks navigation uses the global list",
            "[runtime][unit][workspace][navigation]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(
      FilteredListTarget{.listId = kAllTracksListId, .filterExpression = "$genre = \"Rock\""});

    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == rt::kAllTracksListId);
    CHECK(state.filterExpression == "$genre = \"Rock\"");
  }

  TEST_CASE("WorkspaceService - jumpToAlbum ignores invalid tracks", "[runtime][unit][workspace][navigation]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(ListId{10});
    runtime.workspace().jumpToAlbum(kInvalidTrackId);

    // Invalid track → no-op, history unchanged, still at list 10.
    CHECK(runtime.workspace().canGoBack() == false);
    auto const state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == ListId{10});
  }

  TEST_CASE("WorkspaceService - onFocusedViewChanged emits on focus changes", "[runtime][unit][workspace][focus]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto focusedViewId = kInvalidViewId;
    auto const sub = runtime.workspace().onFocusedViewChanged([&](ViewId id) { focusedViewId = id; });

    runtime.workspace().navigateTo(ListId{10});
    auto activeViewId = runtime.workspace().layoutState().activeViewId;
    CHECK(focusedViewId == activeViewId);
  }

  TEST_CASE("WorkspaceService - deleting a list closes its open views", "[runtime][unit][workspace][lifecycle]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto listId =
      ao::test::requireValue(runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "Test List"}));
    runtime.workspace().navigateTo(listId);

    auto activeViewId = runtime.workspace().layoutState().activeViewId;
    CHECK(activeViewId != kInvalidViewId);

    runtime.library().writer().deleteList(listId);

    auto layout = runtime.workspace().layoutState();
    CHECK(!std::ranges::contains(layout.openViews, activeViewId));
  }

  TEST_CASE("WorkspaceService - jumpToAlbum reveals valid tracks in album presentation",
            "[runtime][unit][workspace][navigation]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto const trackId =
      TrackId{100}; // jumpToAlbum doesn't validate if track exists in library, it just passes the ID to playback

    bool revealCalled = false;
    auto const sub = runtime.playback().onRevealTrackRequested(
      [&](PlaybackService::RevealTrackRequested const& req)
      {
        if (req.trackId == trackId)
        {
          revealCalled = true;
        }
      });

    runtime.workspace().jumpToAlbum(trackId);
    CHECK(revealCalled == true);

    auto state = runtime.views().trackListState(runtime.workspace().layoutState().activeViewId);
    CHECK(state.listId == kAllTracksListId);
    CHECK(state.presentation.id == "albums");
  }

  TEST_CASE("WorkspaceService - invalid navigation targets are ignored", "[runtime][unit][workspace][navigation]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtime = makeRuntime(tempDir);

    runtime.workspace().navigateTo(static_cast<GlobalViewKind>(999));

    CHECK(runtime.workspace().layoutState().activeViewId == kInvalidViewId);
  }
} // namespace ao::rt::test
