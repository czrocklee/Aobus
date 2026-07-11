// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestSupport.h"
#include "test/unit/TestUtils.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSessionState.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace ao::rt::test
{
  using namespace ao::test;

  TEST_CASE("HeadlessShell - navigation and session persistence update layout", "[runtime][unit][headless]")
  {
    auto tempDir = ao::test::TempDir{};
    auto const workspaceConfigPath = std::filesystem::path{tempDir.path()} / "workspace.yaml";

    SECTION("Initial layout is empty")
    {
      auto runtime = makeRuntime(tempDir);
      auto const layout = runtime.workspace().layoutState();
      CHECK(layout.openViews.empty());
      CHECK(layout.activeViewId == kInvalidViewId);
    }

    SECTION("Navigate to list ID creates a view and marks it active")
    {
      auto runtime = makeRuntime(tempDir);
      auto const listId =
        ao::test::requireValue(runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "Headless"}));
      REQUIRE(runtime.workspace().navigateTo(listId));

      auto const layout = runtime.workspace().layoutState();
      REQUIRE(layout.openViews.size() == 1);
      CHECK(layout.activeViewId == layout.openViews.front());

      auto const viewId = layout.activeViewId;
      auto const viewState = runtime.views().trackListState(viewId);
      CHECK(viewState.listId == listId);
    }

    SECTION("Navigate to All Tracks does not reuse a filtered All Tracks view")
    {
      auto runtime = makeRuntime(tempDir);
      auto const filteredView = ao::test::requireValue(runtime.views().createView(
        TrackListViewConfig{.listId = kAllTracksListId, .filterExpression = "$artist ~ \"A\""}));

      runtime.workspace().addView(filteredView.viewId);
      runtime.workspace().setFocusedView(filteredView.viewId);
      REQUIRE(runtime.workspace().navigateTo(GlobalViewKind::AllTracks));

      auto const layout = runtime.workspace().layoutState();
      CHECK(layout.openViews.size() == 2);
      CHECK(layout.activeViewId != filteredView.viewId);

      auto const activeState = runtime.views().trackListState(layout.activeViewId);
      CHECK(activeState.listId == kAllTracksListId);
      CHECK(activeState.filterExpression.empty());
    }

    SECTION("Closing a view updates the layout")
    {
      auto runtime = makeRuntime(tempDir);
      auto const firstListId =
        ao::test::requireValue(runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "First"}));
      auto const secondListId =
        ao::test::requireValue(runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "Second"}));
      REQUIRE(runtime.workspace().navigateTo(firstListId));
      REQUIRE(runtime.workspace().navigateTo(secondListId));

      auto layout1 = runtime.workspace().layoutState();
      REQUIRE(layout1.openViews.size() == 2);
      auto const viewToClose = layout1.openViews.front();
      auto const remainingView = layout1.openViews.back();

      runtime.workspace().closeView(viewToClose);

      auto const layout2 = runtime.workspace().layoutState();
      CHECK(layout2.openViews.size() == 1);
      CHECK(layout2.openViews.front() == remainingView);
      CHECK(layout2.activeViewId == remainingView);
    }

    SECTION("Session persistence works across instances")
    {
      {
        auto runtime = makeRuntime(tempDir);
        auto const firstListId = ao::test::requireValue(
          runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "First saved"}));
        auto const secondListId = ao::test::requireValue(
          runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "Second saved"}));
        REQUIRE(runtime.workspace().navigateTo(firstListId));
        REQUIRE(runtime.workspace().navigateTo(secondListId));
        runtime.workspace().saveSession(runtime.workspaceConfigStore());

        auto loaded = WorkspaceSessionState{};
        auto verifyStore = ConfigStore{workspaceConfigPath};
        REQUIRE(verifyStore.load("workspace", loaded));
        CHECK(loaded.openViews.size() == 2);
      }

      // Create new runtime with same persistence
      auto session2 = makeRuntime(tempDir);

      REQUIRE(session2.workspace().restoreSession(session2.workspaceConfigStore()));

      auto const layout = session2.workspace().layoutState();
      CHECK(layout.openViews.size() == 2);
      CHECK(layout.activeViewId != kInvalidViewId);
    }

    SECTION("Session persistence preserves groupBy across instances")
    {
      {
        auto runtime = makeRuntime(tempDir);
        auto const listId = ao::test::requireValue(
          runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "Grouped saved"}));
        REQUIRE(runtime.workspace().navigateTo(listId));
        auto const viewId = runtime.workspace().layoutState().activeViewId;
        auto const* artistPreset = builtinTrackPresentationPreset("artists");
        REQUIRE(artistPreset != nullptr);
        REQUIRE(runtime.views().setPresentation(viewId, artistPreset->spec));

        auto const savedState = runtime.views().trackListState(viewId);
        CHECK(savedState.groupBy == TrackGroupKey::AlbumArtist);
        CHECK_FALSE(savedState.sortBy.empty());

        runtime.workspace().saveSession(runtime.workspaceConfigStore());

        auto loaded = WorkspaceSessionState{};
        auto verifyStore = ConfigStore{workspaceConfigPath};
        REQUIRE(verifyStore.load("workspace", loaded));
        REQUIRE(loaded.openViews.size() == 1);
        CHECK(loaded.openViews[0].groupBy == TrackGroupKey::AlbumArtist);
      }

      // Restore in new runtime
      auto session2 = makeRuntime(tempDir);

      REQUIRE(session2.workspace().restoreSession(session2.workspaceConfigStore()));

      auto const layout2 = session2.workspace().layoutState();
      REQUIRE(layout2.openViews.size() == 1);
      auto const restoredState = session2.views().trackListState(layout2.openViews[0]);
      CHECK(restoredState.groupBy == TrackGroupKey::AlbumArtist);
      CHECK_FALSE(restoredState.sortBy.empty());
    }

    SECTION("Session persistence preserves groupBy=None")
    {
      {
        auto runtime = makeRuntime(tempDir);
        auto const listId =
          ao::test::requireValue(runtime.library().writer().createList(LibraryWriter::ListDraft{.name = "Flat saved"}));
        REQUIRE(runtime.workspace().navigateTo(listId));
        runtime.workspace().saveSession(runtime.workspaceConfigStore());
      }

      auto session2 = makeRuntime(tempDir);

      REQUIRE(session2.workspace().restoreSession(session2.workspaceConfigStore()));

      auto const layout2 = session2.workspace().layoutState();
      REQUIRE(layout2.openViews.size() == 1);
      auto const restoredState = session2.views().trackListState(layout2.openViews[0]);
      CHECK(restoredState.groupBy == TrackGroupKey::None);
    }
  }
} // namespace ao::rt::test
