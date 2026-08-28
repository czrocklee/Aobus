// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <utility>

namespace ao::rt::test
{
  using namespace ao::test;

  namespace
  {
    ListId createList(AppRuntime& runtime, std::string name)
    {
      return ao::test::requireValue(
        runRuntimeTask(runtime, runtime.library().commands().createList(ListDraft{.name = std::move(name)})));
    }
  } // namespace

  TEST_CASE("HeadlessShell - navigation and session persistence update layout", "[runtime][unit][headless]")
  {
    auto tempDir = ao::test::TempDir{};
    auto const workspaceConfigPath = std::filesystem::path{tempDir.path()} / "workspace.yaml";

    SECTION("Initial layout is empty")
    {
      auto runtimePtr = makeStateOnlyRuntime(tempDir);
      auto const layout = runtimePtr->workspace().snapshot();
      CHECK(layout.openViews.empty());
      CHECK(layout.activeViewId == kInvalidViewId);
    }

    SECTION("Navigate to list ID creates a view and marks it active")
    {
      auto runtimePtr = makeStateOnlyRuntime(tempDir);
      auto const listId = createList(*runtimePtr, "Headless");
      REQUIRE(runtimePtr->workspace().navigate({.target = listId}));

      auto const layout = runtimePtr->workspace().snapshot();
      REQUIRE(layout.openViews.size() == 1);
      CHECK(layout.activeViewId == layout.openViews.front());

      auto const viewId = layout.activeViewId;
      auto const viewState = runtimePtr->views().trackListState(viewId);
      CHECK(viewState.listId == listId);
    }

    SECTION("Navigate to All Tracks does not reuse a filtered All Tracks view")
    {
      auto runtimePtr = makeStateOnlyRuntime(tempDir);
      auto const filteredViewId = ao::test::requireValue(runtimePtr->workspace().navigate({
        .target =
          FilteredListTarget{
            .listId = kAllTracksListId,
            .filterExpression = "$artist ~ \"A\"",
          },
      }));
      REQUIRE(runtimePtr->workspace().navigate({.target = GlobalViewKind::AllTracks}));

      auto const layout = runtimePtr->workspace().snapshot();
      CHECK(layout.openViews.size() == 2);
      CHECK(layout.activeViewId != filteredViewId);

      auto const activeState = runtimePtr->views().trackListState(layout.activeViewId);
      CHECK(activeState.listId == kAllTracksListId);
      CHECK(activeState.filterExpression.empty());
    }

    SECTION("Closing a view updates the layout")
    {
      auto runtimePtr = makeStateOnlyRuntime(tempDir);
      auto const firstListId = createList(*runtimePtr, "First");
      auto const secondListId = createList(*runtimePtr, "Second");
      REQUIRE(runtimePtr->workspace().navigate({.target = firstListId}));
      REQUIRE(runtimePtr->workspace().navigate({.target = secondListId}));

      auto layout1 = runtimePtr->workspace().snapshot();
      REQUIRE(layout1.openViews.size() == 2);
      auto const viewToClose = layout1.openViews.front();
      auto const remainingView = layout1.openViews.back();

      REQUIRE(runtimePtr->workspace().closeView(viewToClose));

      auto const layout2 = runtimePtr->workspace().snapshot();
      CHECK(layout2.openViews.size() == 1);
      CHECK(layout2.openViews.front() == remainingView);
      CHECK(layout2.activeViewId == remainingView);
    }

    SECTION("Session persistence works across instances")
    {
      {
        auto runtimePtr = makeStateOnlyRuntime(tempDir);
        auto const firstListId = createList(*runtimePtr, "First saved");
        auto const secondListId = createList(*runtimePtr, "Second saved");
        REQUIRE(runtimePtr->workspace().navigate({.target = firstListId}));
        REQUIRE(runtimePtr->workspace().navigate({.target = secondListId}));
        runtimePtr->workspace().saveSession(runtimePtr->workspaceConfigStore());

        auto const encoded = ao::test::readFile(workspaceConfigPath);
        CHECK(encoded.contains("presentationVersion: 1"));
        CHECK(encoded.contains("activeViewIndex: 1"));
        CHECK(encoded.contains("group: \"none\""));
        CHECK(encoded.contains("display-track-number"));
      }

      // Create new runtime with same persistence
      auto session2Ptr = makeStateOnlyRuntime(tempDir);

      REQUIRE(session2Ptr->workspace().restoreSession(session2Ptr->workspaceConfigStore()));

      auto const layout = session2Ptr->workspace().snapshot();
      CHECK(layout.openViews.size() == 2);
      CHECK(layout.activeViewId != kInvalidViewId);
    }

    SECTION("Session persistence preserves groupBy across instances")
    {
      {
        auto runtimePtr = makeStateOnlyRuntime(tempDir);
        auto const listId = createList(*runtimePtr, "Grouped saved");
        REQUIRE(runtimePtr->workspace().navigate({.target = listId}));
        auto const viewId = runtimePtr->workspace().snapshot().activeViewId;
        auto const* artistPreset = builtinTrackPresentationPreset("artists");
        REQUIRE(artistPreset != nullptr);
        REQUIRE(runtimePtr->views().setPresentation(viewId, artistPreset->spec));

        auto const savedState = runtimePtr->views().trackListState(viewId);
        CHECK(savedState.groupBy == TrackGroupKey::AlbumArtist);
        CHECK_FALSE(savedState.sortBy.empty());

        runtimePtr->workspace().saveSession(runtimePtr->workspaceConfigStore());
        auto const encoded = ao::test::readFile(workspaceConfigPath);
        CHECK(encoded.contains("group: \"album-artist\""));
        CHECK(encoded.contains("field: \"album-artist\""));
        CHECK(encoded.contains("direction: \"ascending\""));
      }

      // Restore in new runtime
      auto session2Ptr = makeStateOnlyRuntime(tempDir);

      REQUIRE(session2Ptr->workspace().restoreSession(session2Ptr->workspaceConfigStore()));

      auto const layout2 = session2Ptr->workspace().snapshot();
      REQUIRE(layout2.openViews.size() == 1);
      auto const restoredState = session2Ptr->views().trackListState(layout2.openViews[0]);
      CHECK(restoredState.groupBy == TrackGroupKey::AlbumArtist);
      CHECK_FALSE(restoredState.sortBy.empty());
    }

    SECTION("Session persistence preserves groupBy=None")
    {
      {
        auto runtimePtr = makeStateOnlyRuntime(tempDir);
        auto const listId = createList(*runtimePtr, "Flat saved");
        REQUIRE(runtimePtr->workspace().navigate({.target = listId}));
        runtimePtr->workspace().saveSession(runtimePtr->workspaceConfigStore());
      }

      auto session2Ptr = makeStateOnlyRuntime(tempDir);

      REQUIRE(session2Ptr->workspace().restoreSession(session2Ptr->workspaceConfigStore()));

      auto const layout2 = session2Ptr->workspace().snapshot();
      REQUIRE(layout2.openViews.size() == 1);
      auto const restoredState = session2Ptr->views().trackListState(layout2.openViews[0]);
      CHECK(restoredState.groupBy == TrackGroupKey::None);
    }
  }
} // namespace ao::rt::test
