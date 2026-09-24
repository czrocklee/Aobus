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
        runRuntimeTask(runtime, runtime.library().commands().createListAsync(ListDraft{.name = std::move(name)})));
    }
  } // namespace

  TEST_CASE("HeadlessShell - initial layout is empty", "[runtime][unit][headless]")
  {
    auto tempDir = ao::test::TempDir{};
    auto runtimePtr = makeStateOnlyRuntime(tempDir);

    auto const layout = runtimePtr->workspace().snapshot();

    CHECK(layout.openViews.empty());
    CHECK(layout.activeViewId == kInvalidViewId);
  }

  TEST_CASE("HeadlessShell - navigating to a list creates an active view", "[runtime][unit][headless]")
  {
    auto tempDir = ao::test::TempDir{};
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

  TEST_CASE("HeadlessShell - global navigation does not reuse a filtered All Tracks view", "[runtime][unit][headless]")
  {
    auto tempDir = ao::test::TempDir{};
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
    REQUIRE(layout.openViews.size() == 2);
    CHECK(layout.openViews.front() == filteredViewId);
    CHECK(layout.activeViewId == layout.openViews.back());

    auto const filteredState = runtimePtr->views().trackListState(layout.openViews.front());
    CHECK(filteredState.listId == kAllTracksListId);
    CHECK(filteredState.filterExpression == "$artist ~ \"A\"");

    auto const activeState = runtimePtr->views().trackListState(layout.activeViewId);
    CHECK(activeState.listId == kAllTracksListId);
    CHECK(activeState.filterExpression.empty());
  }

  TEST_CASE("HeadlessShell - closing a view updates the active layout", "[runtime][unit][headless]")
  {
    auto tempDir = ao::test::TempDir{};
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
    REQUIRE(layout2.openViews.size() == 1);
    CHECK(layout2.openViews.front() == remainingView);
    CHECK(layout2.activeViewId == remainingView);
  }

  TEST_CASE("HeadlessShell - session persistence restores multiple views and their presentations",
            "[runtime][unit][headless]")
  {
    auto tempDir = ao::test::TempDir{};
    auto const workspaceConfigPath = std::filesystem::path{tempDir.path()} / "workspace.yaml";
    auto firstListId = kInvalidListId;
    auto secondListId = kInvalidListId;
    auto firstPresentation = TrackPresentationSpec{};
    auto secondPresentation = TrackPresentationSpec{};

    {
      auto runtimePtr = makeStateOnlyRuntime(tempDir);
      firstListId = createList(*runtimePtr, "First saved");
      secondListId = createList(*runtimePtr, "Second saved");

      auto const firstViewId = ao::test::requireValue(runtimePtr->workspace().navigate({.target = firstListId}));
      auto const* artistsPreset = builtinTrackPresentationPreset("artists");
      REQUIRE(artistsPreset != nullptr);
      firstPresentation = artistsPreset->spec;
      REQUIRE(runtimePtr->views().setPresentation(firstViewId, firstPresentation));

      auto const secondViewId = ao::test::requireValue(runtimePtr->workspace().navigate({.target = secondListId}));
      auto const* technicalPreset = builtinTrackPresentationPreset("technical");
      REQUIRE(technicalPreset != nullptr);
      secondPresentation = technicalPreset->spec;
      REQUIRE(runtimePtr->views().setPresentation(secondViewId, secondPresentation));

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
    REQUIRE(layout.openViews.size() == 2);
    CHECK(layout.activeViewId == layout.openViews[1]);

    auto const firstState = session2Ptr->views().trackListState(layout.openViews[0]);
    CHECK(firstState.listId == firstListId);
    CHECK(firstState.filterExpression.empty());
    CHECK(firstState.presentation == firstPresentation);

    auto const secondState = session2Ptr->views().trackListState(layout.openViews[1]);
    CHECK(secondState.listId == secondListId);
    CHECK(secondState.filterExpression.empty());
    CHECK(secondState.presentation == secondPresentation);
  }

  TEST_CASE("HeadlessShell - session persistence restores a grouped presentation", "[runtime][unit][headless]")
  {
    auto tempDir = ao::test::TempDir{};
    auto const workspaceConfigPath = std::filesystem::path{tempDir.path()} / "workspace.yaml";
    auto const* artistPreset = builtinTrackPresentationPreset("artists");
    REQUIRE(artistPreset != nullptr);
    auto listId = kInvalidListId;

    {
      auto runtimePtr = makeStateOnlyRuntime(tempDir);
      listId = createList(*runtimePtr, "Grouped saved");
      auto const viewId = ao::test::requireValue(runtimePtr->workspace().navigate({.target = listId}));
      REQUIRE(runtimePtr->views().setPresentation(viewId, artistPreset->spec));

      runtimePtr->workspace().saveSession(runtimePtr->workspaceConfigStore());

      auto const encoded = ao::test::readFile(workspaceConfigPath);
      CHECK(encoded.contains("group: \"album-artist\""));
      CHECK(encoded.contains("field: \"album-artist\""));
      CHECK(encoded.contains("direction: \"ascending\""));
    }

    auto session2Ptr = makeStateOnlyRuntime(tempDir);

    REQUIRE(session2Ptr->workspace().restoreSession(session2Ptr->workspaceConfigStore()));

    auto const layout = session2Ptr->workspace().snapshot();
    REQUIRE(layout.openViews.size() == 1);
    CHECK(layout.activeViewId == layout.openViews.front());
    auto const restoredState = session2Ptr->views().trackListState(layout.openViews.front());
    CHECK(restoredState.listId == listId);
    CHECK(restoredState.filterExpression.empty());
    CHECK(restoredState.groupBy == TrackGroupKey::AlbumArtist);
    CHECK(restoredState.presentation == artistPreset->spec);
  }

  TEST_CASE("HeadlessShell - session persistence restores a flat default presentation", "[runtime][unit][headless]")
  {
    auto tempDir = ao::test::TempDir{};
    auto listId = kInvalidListId;
    auto savedPresentation = TrackPresentationSpec{};

    {
      auto runtimePtr = makeStateOnlyRuntime(tempDir);
      listId = createList(*runtimePtr, "Flat saved");
      auto const viewId = ao::test::requireValue(runtimePtr->workspace().navigate({.target = listId}));
      savedPresentation = runtimePtr->views().trackListState(viewId).presentation;
      REQUIRE(savedPresentation.groupBy == TrackGroupKey::None);

      runtimePtr->workspace().saveSession(runtimePtr->workspaceConfigStore());
    }

    auto session2Ptr = makeStateOnlyRuntime(tempDir);

    REQUIRE(session2Ptr->workspace().restoreSession(session2Ptr->workspaceConfigStore()));

    auto const layout = session2Ptr->workspace().snapshot();
    REQUIRE(layout.openViews.size() == 1);
    CHECK(layout.activeViewId == layout.openViews.front());
    auto const restoredState = session2Ptr->views().trackListState(layout.openViews.front());
    CHECK(restoredState.listId == listId);
    CHECK(restoredState.filterExpression.empty());
    CHECK(restoredState.groupBy == TrackGroupKey::None);
    CHECK(restoredState.presentation == savedPresentation);
  }
} // namespace ao::rt::test
