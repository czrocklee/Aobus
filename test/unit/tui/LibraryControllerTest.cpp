// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/LibraryController.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "tui/LibraryNavigation.h"
#include "tui/TrackPresentationNavigation.h"
#include <ao/CoreIds.h>
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
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tui::test
{
  namespace
  {
    struct LibraryControllerFixture final
    {
      ao::test::TempDir tempDir{};
      std::unique_ptr<rt::AppRuntime> runtimePtr{rt::test::makeStateOnlyRuntime(tempDir)};
      uimodel::TrackPresentationCatalog presentationCatalog{runtimePtr->workspace(), ao::test::englishMessageCatalog()};
      uimodel::ListPresentations listPresentations{presentationCatalog, runtimePtr->library().changes()};

      LibraryController makeController()
      {
        return LibraryController{*runtimePtr, ao::test::englishMessageCatalog(), listPresentations};
      }

      TrackId addTrack(std::string_view title) const
      {
        return rt::test::addRuntimeTrack(*runtimePtr, library::test::TrackSpec{.title = std::string{title}});
      }

      TrackId addTrack(library::test::TrackSpec const& spec) const
      {
        return rt::test::addRuntimeTrack(*runtimePtr, spec);
      }

      ListId addList(std::string name) const
      {
        return ao::test::requireValue(rt::test::runRuntimeTask(
          *runtimePtr, runtimePtr->library().commands().createList(rt::ListDraft{.name = std::move(name)})));
      }
    };

    std::string requireAppliedFilter(LibraryController& controller)
    {
      auto result = controller.applyFilter();
      REQUIRE(result);
      return std::move(*result);
    }
  } // namespace

  TEST_CASE("LibraryController - startup publishes an active all-tracks view", "[tui][regression][library]")
  {
    auto fixture = LibraryControllerFixture{};
    auto const trackId = fixture.addTrack("Needle");

    auto controller = fixture.makeController();

    REQUIRE(controller.activeViewId() != rt::kInvalidViewId);
    auto const workspace = fixture.runtimePtr->workspace().snapshot();
    REQUIRE(workspace.openViews.size() == 1);
    CHECK(workspace.activeViewId == controller.activeViewId());
    CHECK(fixture.runtimePtr->views().trackListState(controller.activeViewId()).listId == rt::kAllTracksListId);
    CHECK_FALSE(fixture.runtimePtr->workspace().canGoBack());
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks()[0].id == trackId);

    controller.setFilterDraft("Needle");
    CHECK(requireAppliedFilter(controller) == "Quick filter matched 1 track");
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks()[0].id == trackId);
  }

  TEST_CASE("LibraryController - fallback navigation uses the shared presentation recommendation",
            "[tui][unit][library][presentation]")
  {
    auto fixture = LibraryControllerFixture{};
    auto catalog =
      uimodel::TrackPresentationCatalog{fixture.runtimePtr->workspace(), ao::test::englishMessageCatalog()};
    auto presentations = uimodel::ListPresentations{catalog, fixture.runtimePtr->library().changes()};

    auto controller = LibraryController{*fixture.runtimePtr, ao::test::englishMessageCatalog(), presentations};

    CHECK(controller.activePresentationId() == "albums");
    CHECK(presentations.snapshot().empty());
  }

  TEST_CASE("LibraryController - new list navigation uses the persisted presentation preference",
            "[tui][unit][library][presentation]")
  {
    auto fixture = LibraryControllerFixture{};
    auto const listId = fixture.addList("Albums");
    auto catalog =
      uimodel::TrackPresentationCatalog{fixture.runtimePtr->workspace(), ao::test::englishMessageCatalog()};
    auto presentations = uimodel::ListPresentations{catalog, fixture.runtimePtr->library().changes()};
    presentations.setPresentationIdForList(rt::kAllTracksListId, "artists");
    presentations.setPresentationIdForList(listId, "albums");

    auto controller = LibraryController{*fixture.runtimePtr, ao::test::englishMessageCatalog(), presentations};

    CHECK(controller.activePresentationId() == "artists");
    auto const listIt = std::ranges::find(controller.libraryEntries(), listId, &LibraryNavEntry::id);
    REQUIRE(listIt != controller.libraryEntries().end());
    auto const listIndex = static_cast<std::int32_t>(listIt - controller.libraryEntries().begin());
    controller.moveFocusedSelection(true, listIndex - controller.selectedList());
    REQUIRE(controller.openSelectedList().opened);
    CHECK(controller.currentListId() == listId);
    CHECK(controller.activePresentationId() == "albums");
  }

  TEST_CASE("LibraryController - successful presentation selection records the list preference",
            "[tui][unit][library][presentation]")
  {
    auto fixture = LibraryControllerFixture{};
    auto catalog =
      uimodel::TrackPresentationCatalog{fixture.runtimePtr->workspace(), ao::test::englishMessageCatalog()};
    auto presentations = uimodel::ListPresentations{catalog, fixture.runtimePtr->library().changes()};
    auto controller = LibraryController{*fixture.runtimePtr, ao::test::englishMessageCatalog(), presentations};

    CHECK(controller.setPresentation("albums") == "View: albums");

    REQUIRE(presentations.snapshot().contains(rt::kAllTracksListId));
    CHECK(presentations.snapshot().at(rt::kAllTracksListId) == "albums");
  }

  TEST_CASE("LibraryController - failed presentation selection does not change the list preference",
            "[tui][regression][library][presentation]")
  {
    auto fixture = LibraryControllerFixture{};
    auto catalog =
      uimodel::TrackPresentationCatalog{fixture.runtimePtr->workspace(), ao::test::englishMessageCatalog()};
    auto presentations = uimodel::ListPresentations{catalog, fixture.runtimePtr->library().changes()};
    presentations.setPresentationIdForList(rt::kAllTracksListId, "albums");
    auto controller = LibraryController{*fixture.runtimePtr, ao::test::englishMessageCatalog(), presentations};
    auto const activeBefore = controller.activePresentationId();

    CHECK(controller.setPresentation("missing-presentation") == "Unknown view missing-presentation");

    CHECK(controller.activePresentationId() == activeBefore);
    CHECK(presentations.snapshot().at(rt::kAllTracksListId) == "albums");
  }

  TEST_CASE("LibraryController - persisted preference does not replace an exact restored active view",
            "[tui][regression][library][presentation]")
  {
    auto fixture = LibraryControllerFixture{};
    auto const* albumsPreset = rt::builtinTrackPresentationPreset("albums");
    REQUIRE(albumsPreset != nullptr);
    auto const activeViewId = ao::test::requireValue(fixture.runtimePtr->workspace().navigate({
      .target = rt::GlobalViewKind::AllTracks,
      .optPresentation =
        rt::NavigationPresentation{
          .mode = rt::NavigationPresentationMode::Override,
          .spec = albumsPreset->spec,
        },
    }));
    auto const workspaceBefore = fixture.runtimePtr->workspace().snapshot();
    auto catalog =
      uimodel::TrackPresentationCatalog{fixture.runtimePtr->workspace(), ao::test::englishMessageCatalog()};
    auto presentations = uimodel::ListPresentations{catalog, fixture.runtimePtr->library().changes()};
    presentations.setPresentationIdForList(rt::kAllTracksListId, "artists");

    auto controller = LibraryController{*fixture.runtimePtr, ao::test::englishMessageCatalog(), presentations};

    CHECK(controller.activeViewId() == activeViewId);
    CHECK(controller.activePresentationId() == "albums");
    CHECK(fixture.runtimePtr->workspace().snapshot() == workspaceBefore);
  }

  TEST_CASE("LibraryController - construction attaches the exact active workspace view", "[tui][regression][library]")
  {
    auto tempDir = ao::test::TempDir{};
    auto firstTrackId = kInvalidTrackId;
    auto secondTrackId = kInvalidTrackId;

    {
      auto runtimePtr = rt::test::makeStateOnlyRuntime(tempDir);
      firstTrackId = rt::test::addRuntimeTrack(*runtimePtr, library::test::TrackSpec{.title = "First"});
      secondTrackId = rt::test::addRuntimeTrack(*runtimePtr, library::test::TrackSpec{.title = "Second"});
      auto const* songsPreset = rt::builtinTrackPresentationPreset("songs");
      auto const* albumsPreset = rt::builtinTrackPresentationPreset("albums");
      REQUIRE(songsPreset != nullptr);
      REQUIRE(albumsPreset != nullptr);
      REQUIRE(runtimePtr->workspace().navigate({
        .target =
          rt::FilteredListTarget{
            .listId = rt::kAllTracksListId,
            .filterExpression = "$title ~ \"First\"",
          },
        .optPresentation =
          rt::NavigationPresentation{
            .mode = rt::NavigationPresentationMode::Override,
            .spec = songsPreset->spec,
          },
      }));
      REQUIRE(runtimePtr->workspace().navigate({
        .target =
          rt::FilteredListTarget{
            .listId = rt::kAllTracksListId,
            .filterExpression = "$title ~ \"Second\"",
          },
        .optPresentation =
          rt::NavigationPresentation{
            .mode = rt::NavigationPresentationMode::Override,
            .spec = albumsPreset->spec,
          },
      }));
      runtimePtr->workspace().saveSession(runtimePtr->workspaceConfigStore());
    }

    auto runtimePtr = rt::test::makeStateOnlyRuntime(tempDir);
    REQUIRE(runtimePtr->workspace().restoreSession(runtimePtr->workspaceConfigStore()));
    rt::test::settleRuntimeCallbacks(*runtimePtr);
    auto const before = runtimePtr->workspace().snapshot();
    REQUIRE(before.openViews.size() == 2);
    auto const expectedActiveViewId = before.activeViewId;
    auto const expectedState = runtimePtr->views().trackListState(expectedActiveViewId);
    REQUIRE(expectedState.filterExpression == "$title ~ \"Second\"");

    auto presentationCatalog =
      uimodel::TrackPresentationCatalog{runtimePtr->workspace(), ao::test::englishMessageCatalog()};
    auto listPresentations = uimodel::ListPresentations{presentationCatalog, runtimePtr->library().changes()};
    auto controller = LibraryController{*runtimePtr, ao::test::englishMessageCatalog(), listPresentations};

    CHECK(controller.activeViewId() == expectedActiveViewId);
    CHECK(runtimePtr->workspace().snapshot() == before);
    CHECK(controller.currentListId() == rt::kAllTracksListId);
    auto const allTracksIt = std::ranges::find(controller.libraryEntries(), rt::kAllTracksListId, &LibraryNavEntry::id);
    REQUIRE(allTracksIt != controller.libraryEntries().end());
    CHECK(controller.selectedList() == static_cast<std::int32_t>(allTracksIt - controller.libraryEntries().begin()));
    CHECK(controller.filterDraft() == expectedState.filterExpression);
    CHECK(controller.activePresentationId() == "albums");
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks()[0].id == secondTrackId);
    CHECK(controller.tracks()[0].id != firstTrackId);
    auto const selection = runtimePtr->views().trackListState(expectedActiveViewId).selection;
    REQUIRE(selection.size() == 1);
    CHECK(selection[0] == secondTrackId);
  }

  TEST_CASE("LibraryController - construction keeps a valid empty restored view", "[tui][regression][library]")
  {
    auto tempDir = ao::test::TempDir{};

    {
      auto runtimePtr = rt::test::makeStateOnlyRuntime(tempDir);
      REQUIRE(rt::test::addRuntimeTrack(*runtimePtr, library::test::TrackSpec{.title = "Present"}) != kInvalidTrackId);
      REQUIRE(runtimePtr->workspace().navigate({
        .target =
          rt::FilteredListTarget{
            .listId = rt::kAllTracksListId,
            .filterExpression = "$title ~ \"Absent\"",
          },
      }));
      runtimePtr->workspace().saveSession(runtimePtr->workspaceConfigStore());
    }

    auto runtimePtr = rt::test::makeStateOnlyRuntime(tempDir);
    REQUIRE(runtimePtr->workspace().restoreSession(runtimePtr->workspaceConfigStore()));
    rt::test::settleRuntimeCallbacks(*runtimePtr);
    auto const before = runtimePtr->workspace().snapshot();
    REQUIRE(before.openViews.size() == 1);
    auto const expectedActiveViewId = before.activeViewId;

    auto presentationCatalog =
      uimodel::TrackPresentationCatalog{runtimePtr->workspace(), ao::test::englishMessageCatalog()};
    auto listPresentations = uimodel::ListPresentations{presentationCatalog, runtimePtr->library().changes()};
    auto controller = LibraryController{*runtimePtr, ao::test::englishMessageCatalog(), listPresentations};

    CHECK(controller.activeViewId() == expectedActiveViewId);
    CHECK(runtimePtr->workspace().snapshot() == before);
    CHECK(controller.filterDraft() == "$title ~ \"Absent\"");
    CHECK(controller.tracks().empty());
  }

  TEST_CASE("LibraryController - reload preserves the active view configuration", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    auto const matchingTrackId = fixture.addTrack("Needle");
    fixture.addTrack("Other");

    auto controller = fixture.makeController();
    controller.setFilterDraft("Needle");
    CHECK(requireAppliedFilter(controller) == "Quick filter matched 1 track");
    CHECK(controller.setPresentation("albums") == "View: albums");
    REQUIRE(controller.tracks().size() == 1);
    auto const activeViewId = controller.activeViewId();
    auto const workspaceBefore = fixture.runtimePtr->workspace().snapshot();
    auto const viewBefore = fixture.runtimePtr->views().trackListState(activeViewId);

    CHECK(controller.reloadActiveList() == "Reloaded 1 track");

    CHECK(controller.activeViewId() == activeViewId);
    CHECK(fixture.runtimePtr->workspace().snapshot() == workspaceBefore);
    CHECK(fixture.runtimePtr->views().trackListState(activeViewId).filterExpression == viewBefore.filterExpression);
    CHECK(fixture.runtimePtr->views().trackListState(activeViewId).presentation == viewBefore.presentation);
    CHECK(controller.filterDraft() == "Needle");
    CHECK(controller.activePresentationId() == "albums");
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks()[0].id == matchingTrackId);
  }

  TEST_CASE("LibraryController - track-only reload preserves chooser highlights", "[tui][regression][library]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("Initial");
    auto const otherListId = fixture.addList("Other");
    auto controller = fixture.makeController();
    auto const listIt = std::ranges::find(controller.libraryEntries(), otherListId, &LibraryNavEntry::id);
    REQUIRE(listIt != controller.libraryEntries().end());
    auto const selectedList = static_cast<std::int32_t>(listIt - controller.libraryEntries().begin());
    controller.moveFocusedSelection(true, selectedList - controller.selectedList());
    REQUIRE(controller.presentationEntries().size() > 1);
    auto const activePresentationId = controller.activePresentationId();
    auto const selectedPresentation = controller.selectedPresentation() == 0 ? 1 : 0;
    REQUIRE(controller.setSelectedPresentation(selectedPresentation));

    fixture.addTrack("Added");
    rt::test::settleRuntimeCallbacks(*fixture.runtimePtr);

    CHECK(controller.selectedList() == selectedList);
    CHECK(controller.selectedPresentation() == selectedPresentation);
    CHECK(controller.currentListId() == rt::kAllTracksListId);
    CHECK(controller.activePresentationId() == activePresentationId);
    CHECK(controller.tracks().size() == 2);
  }

  TEST_CASE("LibraryController - reload falls back when the active view disappeared",
            "[tui][regression][library][lifecycle]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("Needle");
    fixture.addTrack("Other");
    auto controller = fixture.makeController();
    controller.setFilterDraft("Needle");
    REQUIRE(controller.applyFilter());
    auto const closedViewId = controller.activeViewId();
    REQUIRE(fixture.runtimePtr->workspace().closeView(closedViewId));

    CHECK(controller.reloadActiveList() == "Reloaded 2 tracks");

    CHECK(controller.activeViewId() != closedViewId);
    CHECK(controller.currentListId() == rt::kAllTracksListId);
    CHECK(controller.filterDraft().empty());
    REQUIRE(controller.tracks().size() == 2);
    CHECK(fixture.runtimePtr->workspace().snapshot().activeViewId == controller.activeViewId());
  }

  TEST_CASE("LibraryController - list deletion recovers without observer-order assumptions",
            "[tui][regression][library][lifecycle]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("Needle");
    auto const temporaryListId = fixture.addList("Temporary");
    auto catalog =
      uimodel::TrackPresentationCatalog{fixture.runtimePtr->workspace(), ao::test::englishMessageCatalog()};
    auto presentations = uimodel::ListPresentations{catalog, fixture.runtimePtr->library().changes()};
    presentations.setPresentationIdForList(temporaryListId, "albums");
    auto columnLayouts = uimodel::TrackColumnLayouts{fixture.runtimePtr->library().changes()};
    columnLayouts.updateLayout(
      temporaryListId, {uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 17, .weight = -1.0}});
    auto controller = LibraryController{*fixture.runtimePtr, ao::test::englishMessageCatalog(), presentations};
    auto const allTracksViewId = controller.activeViewId();
    auto const listIt = std::ranges::find(controller.libraryEntries(), temporaryListId, &LibraryNavEntry::id);
    REQUIRE(listIt != controller.libraryEntries().end());
    controller.moveFocusedSelection(true, static_cast<std::int32_t>(listIt - controller.libraryEntries().begin()));
    REQUIRE(controller.openSelectedList().opened);
    REQUIRE(controller.currentListId() == temporaryListId);

    REQUIRE(rt::test::runRuntimeTask(
      *fixture.runtimePtr, fixture.runtimePtr->library().commands().deleteList(temporaryListId)));
    rt::test::settleRuntimeCallbacks(*fixture.runtimePtr);

    CHECK(controller.activeViewId() == allTracksViewId);
    CHECK(controller.currentListId() == rt::kAllTracksListId);
    CHECK(fixture.runtimePtr->workspace().snapshot().activeViewId == allTracksViewId);
    CHECK_FALSE(presentations.snapshot().contains(temporaryListId));
    CHECK_FALSE(columnLayouts.snapshot().contains(temporaryListId));
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks()[0].row.title == "Needle");
  }

  TEST_CASE("LibraryController - expression filters report expression mode", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    auto const trackId = fixture.addTrack("Needle");
    fixture.addTrack("Other");

    auto controller = fixture.makeController();
    controller.setFilterDraft("$title ~ \"Needle\"");

    CHECK(requireAppliedFilter(controller) == "Expression filter matched 1 track");
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks()[0].id == trackId);
  }

  TEST_CASE("LibraryController - exposes and clears transient expression errors", "[tui][unit][library][filter]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("Needle");

    auto controller = fixture.makeController();
    controller.setFilterDraft("$artist =");

    REQUIRE(controller.applyFilter());
    CHECK(controller.filterError().contains("Filter error:"));

    controller.setFilterDraft("Needle");
    REQUIRE(controller.applyFilter());
    CHECK(controller.filterError().empty());
  }

  TEST_CASE("LibraryController - opening another list clears the previous view filter error",
            "[tui][regression][library][filter]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("Needle");
    auto const cleanListId = fixture.addList("Clean");

    auto controller = fixture.makeController();
    controller.setFilterDraft("$artist =");
    REQUIRE(controller.applyFilter());
    REQUIRE(controller.filterError().contains("Filter error:"));

    auto const listIt = std::ranges::find(controller.libraryEntries(), cleanListId, &LibraryNavEntry::id);
    REQUIRE(listIt != controller.libraryEntries().end());
    controller.moveFocusedSelection(true, static_cast<std::int32_t>(listIt - controller.libraryEntries().begin()));

    auto const opened = controller.openSelectedList();

    REQUIRE(opened.opened);
    CHECK(controller.currentListId() == cleanListId);
    CHECK(controller.filterDraft().empty());
    CHECK(controller.filterError().empty());
  }

  TEST_CASE("LibraryController - filter error preserves visible controller state", "[tui][regression][library]")
  {
    auto fixture = LibraryControllerFixture{};
    auto const firstTrackId = fixture.addTrack("Needle");
    fixture.addTrack("Other");

    auto controller = fixture.makeController();
    auto const activeViewId = controller.activeViewId();
    auto const selectedTrack = controller.selectedTrack();
    auto const sectionCount = controller.sections().size();
    controller.setFilterDraft("Needle");
    REQUIRE(fixture.runtimePtr->workspace().closeView(activeViewId));

    auto result = controller.applyFilter();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
    CHECK(controller.filterError().contains("Filter error:"));
    CHECK(controller.filterDraft() == "Needle");
    CHECK(controller.activeViewId() == activeViewId);
    CHECK(controller.selectedTrack() == selectedTrack);
    CHECK(controller.sections().size() == sectionCount);
    REQUIRE(controller.tracks().size() == 2);
    CHECK(controller.tracks().front().id == firstTrackId);
    CHECK(controller.activePresentation() == rt::defaultTrackPresentationSpec());
  }

  TEST_CASE("LibraryController - empty track views clear the published selection", "[tui][regression][library]")
  {
    auto fixture = LibraryControllerFixture{};
    auto const trackId = fixture.addTrack("Needle");

    auto controller = fixture.makeController();
    auto const activeViewId = controller.activeViewId();
    auto const selectionBefore = fixture.runtimePtr->views().trackListState(activeViewId).selection;
    REQUIRE(selectionBefore.size() == 1);
    CHECK(selectionBefore.front() == trackId);
    controller.setFilterDraft("not-present");

    CHECK(requireAppliedFilter(controller) == "Quick filter matched 0 tracks");
    CHECK(controller.tracks().empty());
    CHECK(fixture.runtimePtr->views().trackListState(activeViewId).selection.empty());
    CHECK(fixture.runtimePtr->workspace().snapshot().activeViewId == activeViewId);

    auto selected = controller.selectedTrackView();
    CHECK(selected.track == nullptr);
    CHECK(selected.coverArtId == kInvalidResourceId);
  }

  TEST_CASE("LibraryController - revealTrack selects a visible track", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("First");
    auto const secondId = fixture.addTrack("Second");

    auto controller = fixture.makeController();

    CHECK(controller.revealTrack(secondId) == "Revealed Second");
    CHECK(controller.selectedTrack() == 1);
    REQUIRE(controller.selectedTrackView().track != nullptr);
    CHECK(controller.selectedTrackView().track->id == secondId);
  }

  TEST_CASE("LibraryController - revealTrack reports unavailable targets", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("First");
    auto const hiddenId = fixture.addTrack("Hidden");

    auto controller = fixture.makeController();

    CHECK(controller.revealTrack(kInvalidTrackId) == "No current track");

    controller.setFilterDraft("First");
    CHECK(requireAppliedFilter(controller) == "Quick filter matched 1 track");
    CHECK(controller.revealTrack(hiddenId) == "Current track is not in this view");
    CHECK(controller.selectedTrack() == 0);
  }

  TEST_CASE("LibraryController - revealTrack selects a track after presentation reorder", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    auto const lastId = fixture.addTrack(library::test::TrackSpec{.title = "B One",
                                                                  .artist = "Artist",
                                                                  .album = "Album B",
                                                                  .albumArtist = "Artist",
                                                                  .uri = "b1.flac",
                                                                  .year = 2021});
    auto const targetId = fixture.addTrack(library::test::TrackSpec{.title = "A One",
                                                                    .artist = "Artist",
                                                                    .album = "Album A",
                                                                    .albumArtist = "Artist",
                                                                    .uri = "a1.flac",
                                                                    .year = 2020});
    auto const secondId = fixture.addTrack(library::test::TrackSpec{.title = "A Two",
                                                                    .artist = "Artist",
                                                                    .album = "Album A",
                                                                    .albumArtist = "Artist",
                                                                    .uri = "a2.flac",
                                                                    .year = 2020});

    auto controller = fixture.makeController();
    REQUIRE(controller.setPresentation("albums") == "View: albums");
    REQUIRE(controller.tracks().size() == 3);
    CHECK(controller.tracks()[0].id == targetId);
    CHECK(controller.tracks()[1].id == secondId);
    CHECK(controller.tracks()[2].id == lastId);

    CHECK(controller.revealTrack(targetId) == "Revealed A One");
    CHECK(controller.selectedTrack() == 0);
    REQUIRE(controller.selectedTrackView().track != nullptr);
    CHECK(controller.selectedTrackView().track->id == targetId);
  }

  TEST_CASE("LibraryController - setPresentation applies active workspace presentation", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("First");

    auto controller = fixture.makeController();

    CHECK(controller.setPresentation("albums") == "View: albums");
    CHECK(fixture.runtimePtr->views().trackListState(controller.activeViewId()).presentation.id == "albums");
    CHECK(controller.setPresentation("missing-preset") == "Unknown view missing-preset");
    CHECK(fixture.runtimePtr->views().trackListState(controller.activeViewId()).presentation.id == "albums");
  }

  TEST_CASE("LibraryController - construction reads custom presets already restored in workspace",
            "[tui][regression][library]")
  {
    auto tempDir = ao::test::TempDir{};
    auto customSpec = rt::defaultTrackPresentationSpec();
    customSpec.id = "custom-restored";

    {
      auto runtimePtr = rt::test::makeStateOnlyRuntime(tempDir);
      REQUIRE(runtimePtr->workspace().addCustomPreset(rt::CustomTrackPresentationPreset{
        .label = "Restored Custom",
        .basePresetId = "songs",
        .spec = customSpec,
      }));
      runtimePtr->workspace().saveSession(runtimePtr->workspaceConfigStore());
    }

    auto runtimePtr = rt::test::makeStateOnlyRuntime(tempDir);
    REQUIRE(runtimePtr->workspace().restoreSession(runtimePtr->workspaceConfigStore()));
    rt::test::settleRuntimeCallbacks(*runtimePtr);

    auto presentationCatalog =
      uimodel::TrackPresentationCatalog{runtimePtr->workspace(), ao::test::englishMessageCatalog()};
    auto listPresentations = uimodel::ListPresentations{presentationCatalog, runtimePtr->library().changes()};
    auto controller = LibraryController{*runtimePtr, ao::test::englishMessageCatalog(), listPresentations};

    auto const entryIt =
      std::ranges::find(controller.presentationEntries(), customSpec.id, &TrackPresentationNavEntry::id);
    REQUIRE(entryIt != controller.presentationEntries().end());
    CHECK(entryIt->label == "Restored Custom");
  }

  TEST_CASE("LibraryController - presentation list follows custom preset changes", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("First");

    auto controller = fixture.makeController();
    auto customSpec = rt::defaultTrackPresentationSpec();
    customSpec.id = "custom-songs";

    auto const initialCount = controller.presentationEntries().size();
    REQUIRE(fixture.runtimePtr->workspace().addCustomPreset(rt::CustomTrackPresentationPreset{
      .label = "Custom Songs",
      .basePresetId = "songs",
      .spec = customSpec,
    }));
    rt::test::settleRuntimeCallbacks(*fixture.runtimePtr);

    REQUIRE(controller.presentationEntries().size() == initialCount + 1);
    CHECK(controller.presentationEntries().back().id == "custom-songs");
    REQUIRE(controller.selectedPresentation() >= 0);
    CHECK(controller.presentationEntries()[static_cast<std::size_t>(controller.selectedPresentation())].id ==
          controller.activePresentationId());

    CHECK_FALSE(controller.setSelectedPresentation(-1));
    CHECK_FALSE(controller.setSelectedPresentation(static_cast<std::int32_t>(controller.presentationEntries().size())));
    REQUIRE(controller.setSelectedPresentation(static_cast<std::int32_t>(controller.presentationEntries().size()) - 1));

    CHECK(controller.selectSelectedPresentation() == "View: custom-songs");
    CHECK(fixture.runtimePtr->views().trackListState(controller.activeViewId()).presentation.id == "custom-songs");
    CHECK(controller.selectedPresentation() == static_cast<std::int32_t>(controller.presentationEntries().size()) - 1);
  }

  TEST_CASE("LibraryController - setPresentation preserves selected track identity", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    auto const olderId = fixture.addTrack(library::test::TrackSpec{
      .title = "Older", .artist = "Same Artist", .album = "Z Album", .uri = "older.flac", .year = 2000});
    fixture.addTrack(library::test::TrackSpec{
      .title = "Newer", .artist = "Same Artist", .album = "A Album", .uri = "newer.flac", .year = 2025});

    auto controller = fixture.makeController();
    REQUIRE(controller.tracks().size() == 2);
    REQUIRE(controller.tracks()[1].id == olderId);

    controller.moveFocusedSelection(false, 1);
    REQUIRE(controller.selectedTrackView().track != nullptr);
    REQUIRE(controller.selectedTrackView().track->id == olderId);

    CHECK(controller.setPresentation("artists") == "View: artists");
    REQUIRE(controller.selectedTrackView().track != nullptr);
    CHECK(controller.selectedTrackView().track->id == olderId);
    CHECK(controller.selectedTrack() == 0);
  }

  TEST_CASE("LibraryController - album presentation exposes projection sections", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack(library::test::TrackSpec{
      .title = "A One", .artist = "Artist", .album = "Album A", .albumArtist = "Artist", .year = 2020});
    fixture.addTrack(library::test::TrackSpec{
      .title = "A Two", .artist = "Artist", .album = "Album A", .albumArtist = "Artist", .year = 2020});
    fixture.addTrack(library::test::TrackSpec{
      .title = "B One", .artist = "Artist", .album = "Album B", .albumArtist = "Artist", .year = 2021});

    auto controller = fixture.makeController();

    CHECK(controller.setPresentation("albums") == "View: albums");
    REQUIRE(controller.sections().size() == 2);
    CHECK(controller.sections()[0].primaryText == "Album A");
    CHECK(controller.sections()[0].secondaryText == "Artist");
    CHECK(controller.sections()[0].tertiaryText == "2020");
    CHECK(controller.sections()[0].rowBegin == 0);
    CHECK(controller.sections()[0].rowCount == 2);
    CHECK(controller.sections()[1].primaryText == "Album B");
    CHECK(controller.sections()[1].rowBegin == 2);
    CHECK(controller.sections()[1].rowCount == 1);
  }

  TEST_CASE("LibraryController - section jumps keep track selection flat", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack(
      library::test::TrackSpec{.title = "A One", .artist = "Artist", .album = "Album A", .albumArtist = "Artist"});
    fixture.addTrack(
      library::test::TrackSpec{.title = "A Two", .artist = "Artist", .album = "Album A", .albumArtist = "Artist"});
    fixture.addTrack(
      library::test::TrackSpec{.title = "B One", .artist = "Artist", .album = "Album B", .albumArtist = "Artist"});

    auto controller = fixture.makeController();
    REQUIRE(controller.setPresentation("albums") == "View: albums");
    REQUIRE(controller.sections().size() == 2);

    CHECK(controller.jumpToAdjacentSection(1) == "Section: Album B");
    CHECK(controller.selectedTrack() == 2);

    CHECK(controller.jumpToAdjacentSection(-1) == "Section: Album A");
    CHECK(controller.selectedTrack() == 0);
  }

  TEST_CASE("LibraryController - section selection reports empty and invalid states", "[tui][unit][library]")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("First");

    auto controller = fixture.makeController();
    REQUIRE(controller.setPresentation("songs") == "View: songs");
    REQUIRE(controller.sections().empty());

    CHECK(controller.jumpToAdjacentSection(1) == "No sections in this view");
    CHECK(controller.selectSection(0) == "No section selected");
    CHECK(controller.selectSection(-1) == "No section selected");
    CHECK(controller.selectedTrack() == 0);
  }
} // namespace ao::tui::test
