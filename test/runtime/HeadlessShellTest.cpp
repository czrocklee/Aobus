// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/TestFixtureSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
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
    ListId createList(AppRuntimeOwner& owner, std::string name)
    {
      return ao::test::requireValue(
        runRuntimeTask(owner.runtime(), owner.libraryCommands().createListAsync(ListDraft{.name = std::move(name)})));
    }
  } // namespace

  TEST_CASE("HeadlessShell - initial layout is empty")
  {
    auto tempDir = ao::test::TempDir{};
    auto ownerPtr = makeStateOnlyRuntime(tempDir);

    auto const layout = ownerPtr->runtime().workspace().snapshot();

    CHECK(layout.openViews.empty());
    CHECK(layout.activeViewId == kInvalidViewId);
  }

  TEST_CASE("HeadlessShell - navigating to a list creates an active view")
  {
    auto tempDir = ao::test::TempDir{};
    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    auto const listId = createList(*ownerPtr, "Headless");

    REQUIRE(runtime.workspace().navigate({.target = listId}));

    auto const layout = runtime.workspace().snapshot();
    REQUIRE(layout.openViews.size() == 1);
    CHECK(layout.activeViewId == layout.openViews.front());

    auto const viewId = layout.activeViewId;
    auto const viewState = runtime.views().trackListState(viewId);
    CHECK(viewState.listId == listId);
  }

  TEST_CASE("HeadlessShell - global navigation does not reuse a filtered All Tracks view")
  {
    auto tempDir = ao::test::TempDir{};
    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    auto const filteredViewId = ao::test::requireValue(runtime.workspace().navigate({
      .target =
        FilteredListTarget{
          .listId = kAllTracksListId,
          .filterExpression = "$artist ~ \"A\"",
        },
    }));

    REQUIRE(runtime.workspace().navigate({.target = GlobalViewKind::AllTracks}));

    auto const layout = runtime.workspace().snapshot();
    REQUIRE(layout.openViews.size() == 2);
    CHECK(layout.openViews.front() == filteredViewId);
    CHECK(layout.activeViewId == layout.openViews.back());

    auto const filteredState = runtime.views().trackListState(layout.openViews.front());
    CHECK(filteredState.listId == kAllTracksListId);
    CHECK(filteredState.filterExpression == "$artist ~ \"A\"");

    auto const activeState = runtime.views().trackListState(layout.activeViewId);
    CHECK(activeState.listId == kAllTracksListId);
    CHECK(activeState.filterExpression.empty());
  }

  TEST_CASE("HeadlessShell - closing a view updates the active layout")
  {
    auto tempDir = ao::test::TempDir{};
    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& runtime = ownerPtr->runtime();
    auto const firstListId = createList(*ownerPtr, "First");
    auto const secondListId = createList(*ownerPtr, "Second");
    REQUIRE(runtime.workspace().navigate({.target = firstListId}));
    REQUIRE(runtime.workspace().navigate({.target = secondListId}));

    auto layout1 = runtime.workspace().snapshot();
    REQUIRE(layout1.openViews.size() == 2);
    auto const viewToClose = layout1.openViews.front();
    auto const remainingView = layout1.openViews.back();

    REQUIRE(runtime.workspace().closeView(viewToClose));

    auto const layout2 = runtime.workspace().snapshot();
    REQUIRE(layout2.openViews.size() == 1);
    CHECK(layout2.openViews.front() == remainingView);
    CHECK(layout2.activeViewId == remainingView);
  }

  TEST_CASE("HeadlessShell - session persistence restores multiple views and their presentations")
  {
    auto tempDir = ao::test::TempDir{};
    auto const workspaceConfigPath = std::filesystem::path{tempDir.path()} / "workspace.yaml";
    auto firstListId = kInvalidListId;
    auto secondListId = kInvalidListId;
    auto firstPresentation = TrackPresentationSpec{};
    auto secondPresentation = TrackPresentationSpec{};

    {
      auto ownerPtr = makeStateOnlyRuntime(tempDir);
      auto& runtime = ownerPtr->runtime();
      firstListId = createList(*ownerPtr, "First saved");
      secondListId = createList(*ownerPtr, "Second saved");

      REQUIRE(runtime.workspace().navigate({.target = firstListId}));
      firstPresentation = builtinTrackPresentationPreset(BuiltinPreset::Artists).spec;
      REQUIRE(runtime.workspace().setActivePresentation(firstPresentation, {.recordHistory = false}));

      REQUIRE(runtime.workspace().navigate({.target = secondListId}));
      secondPresentation = builtinTrackPresentationPreset(BuiltinPreset::Technical).spec;
      REQUIRE(runtime.workspace().setActivePresentation(secondPresentation, {.recordHistory = false}));

      runtime.workspace().saveSession(runtime.workspaceConfigStore());

      auto const encoded = ao::test::readFile(workspaceConfigPath);
      CHECK(encoded.contains("presentationVersion: 3"));
      CHECK(encoded.contains("activeViewIndex: 1"));
      CHECK(encoded.contains("group: \"none\""));
      CHECK(encoded.contains("display-track-number"));
    }

    // Create new runtime with same persistence
    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& restoredRuntime = ownerPtr->runtime();

    REQUIRE(restoredRuntime.workspace().restoreSession(restoredRuntime.workspaceConfigStore()));

    auto const layout = restoredRuntime.workspace().snapshot();
    REQUIRE(layout.openViews.size() == 2);
    CHECK(layout.activeViewId == layout.openViews[1]);

    auto const firstState = restoredRuntime.views().trackListState(layout.openViews[0]);
    CHECK(firstState.listId == firstListId);
    CHECK(firstState.filterExpression.empty());
    CHECK(firstState.presentation == firstPresentation);

    auto const secondState = restoredRuntime.views().trackListState(layout.openViews[1]);
    CHECK(secondState.listId == secondListId);
    CHECK(secondState.filterExpression.empty());
    CHECK(secondState.presentation == secondPresentation);
  }

  TEST_CASE("HeadlessShell - session persistence restores a grouped presentation")
  {
    auto tempDir = ao::test::TempDir{};
    auto const workspaceConfigPath = std::filesystem::path{tempDir.path()} / "workspace.yaml";
    auto const& artistSpec = builtinTrackPresentationPreset(BuiltinPreset::Artists).spec;
    auto listId = kInvalidListId;

    {
      auto ownerPtr = makeStateOnlyRuntime(tempDir);
      auto& runtime = ownerPtr->runtime();
      listId = createList(*ownerPtr, "Grouped saved");
      REQUIRE(runtime.workspace().navigate({.target = listId}));
      REQUIRE(runtime.workspace().setActivePresentation(artistSpec, {.recordHistory = false}));

      runtime.workspace().saveSession(runtime.workspaceConfigStore());

      auto const encoded = ao::test::readFile(workspaceConfigPath);
      CHECK(encoded.contains("group: \"album-artist\""));
      CHECK(encoded.contains("field: \"album-artist\""));
      CHECK(encoded.contains("direction: \"ascending\""));
    }

    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& restoredRuntime = ownerPtr->runtime();

    REQUIRE(restoredRuntime.workspace().restoreSession(restoredRuntime.workspaceConfigStore()));

    auto const layout = restoredRuntime.workspace().snapshot();
    REQUIRE(layout.openViews.size() == 1);
    CHECK(layout.activeViewId == layout.openViews.front());
    auto const restoredState = restoredRuntime.views().trackListState(layout.openViews.front());
    CHECK(restoredState.listId == listId);
    CHECK(restoredState.filterExpression.empty());
    CHECK(restoredState.groupBy == TrackGroupKey::AlbumArtist);
    CHECK(restoredState.presentation == artistSpec);
  }

  TEST_CASE("HeadlessShell - session persistence restores a flat default presentation")
  {
    auto tempDir = ao::test::TempDir{};
    auto listId = kInvalidListId;
    auto savedPresentation = TrackPresentationSpec{};

    {
      auto ownerPtr = makeStateOnlyRuntime(tempDir);
      auto& runtime = ownerPtr->runtime();
      listId = createList(*ownerPtr, "Flat saved");
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = listId}));
      savedPresentation = runtime.views().trackListState(viewId).presentation;
      REQUIRE(savedPresentation.groupBy == TrackGroupKey::None);

      runtime.workspace().saveSession(runtime.workspaceConfigStore());
    }

    auto ownerPtr = makeStateOnlyRuntime(tempDir);
    auto& restoredRuntime = ownerPtr->runtime();

    REQUIRE(restoredRuntime.workspace().restoreSession(restoredRuntime.workspaceConfigStore()));

    auto const layout = restoredRuntime.workspace().snapshot();
    REQUIRE(layout.openViews.size() == 1);
    CHECK(layout.activeViewId == layout.openViews.front());
    auto const restoredState = restoredRuntime.views().trackListState(layout.openViews.front());
    CHECK(restoredState.listId == listId);
    CHECK(restoredState.filterExpression.empty());
    CHECK(restoredState.groupBy == TrackGroupKey::None);
    CHECK(restoredState.presentation == savedPresentation);
  }
} // namespace ao::rt::test
