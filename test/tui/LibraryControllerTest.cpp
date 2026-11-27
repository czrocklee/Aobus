// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/LibraryController.h"

#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include "tui/LibraryNavigation.h"
#include "tui/ListNavigationModel.h"
#include "tui/TrackListEntry.h"
#include "tui/TrackPresentationNavigation.h"
#include <ao/CoreIds.h>
#include <ao/i18n/IcuTextOrdering.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/utility/Path.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    struct LibraryControllerFixture final
    {
      ao::test::TempDir tempDir{};
      std::unique_ptr<rt::AppRuntimeOwner> runtimeOwnerPtr{rt::test::makeStateOnlyRuntime(tempDir)};
      uimodel::TrackPresentationCatalog presentationCatalog{runtime().workspace(), ao::test::englishMessageCatalog()};
      uimodel::ListPresentations listPresentations{presentationCatalog, runtime().library().changes()};
      uimodel::TrackColumnLayouts columnLayouts{runtime().library().changes()};

      rt::AppRuntime& runtime() const noexcept { return runtimeOwnerPtr->runtime(); }

      LibraryController makeController()
      {
        return LibraryController{runtime().library(),
                                 runtime().views(),
                                 runtime().workspace(),
                                 ao::test::englishMessageCatalog(),
                                 listPresentations,
                                 columnLayouts};
      }

      TrackId addTrack(std::string_view title) const
      {
        return rt::test::addRuntimeTrack(*runtimeOwnerPtr, library::test::TrackSpec{.title = std::string{title}});
      }

      TrackId addTrack(library::test::TrackSpec const& spec) const
      {
        return rt::test::addRuntimeTrack(*runtimeOwnerPtr, spec);
      }

      ListId addList(std::string name) const
      {
        return ao::test::requireValue(rt::test::runRuntimeTask(
          runtime(), runtimeOwnerPtr->libraryCommands().createListAsync(rt::ListDraft{.name = std::move(name)})));
      }
    };

    std::string requireAppliedFilter(LibraryController& controller)
    {
      auto res = controller.applyFilter();
      REQUIRE(res);
      return std::move(*res);
    }
  } // namespace

  TEST_CASE("LibraryController - live locale change retains focused and marked track identities")
  {
    auto fixture = LibraryControllerFixture{};
    auto const umlaut = fixture.addTrack("ä");
    auto const zed = fixture.addTrack("z");
    fixture.runtime().setTextOrderingPolicy(
      std::shared_ptr<rt::TextOrderingPolicy const>{ao::test::requireValue(i18n::createIcuTextOrderingPolicy("de"))});
    auto controller = fixture.makeController();
    REQUIRE(controller.setPresentation(rt::BuiltinPreset::Songs));
    controller.setSelectedTrackIndex(0);
    REQUIRE(controller.selectedTrackView().track->id == umlaut);
    controller.toggleFocusedMark();
    auto const viewId = controller.activeViewId();
    fixture.runtime().setTextOrderingPolicy(
      std::shared_ptr<rt::TextOrderingPolicy const>{ao::test::requireValue(i18n::createIcuTextOrderingPolicy("sv"))});
    controller.setTextCatalog(ao::test::messageCatalog("zh-Hans"));
    CHECK(controller.activeViewId() == viewId);
    CHECK(controller.selectedTrackView().track->id == umlaut);
    CHECK(controller.markedIds() == std::unordered_set<TrackId>{umlaut});
    REQUIRE(controller.tracks().size() == 2);
    CHECK(controller.tracks().front().id == zed);
    CHECK(controller.currentListTitle() != "All Tracks");
  }

  TEST_CASE("LibraryController - startup publishes an active all-tracks view")
  {
    auto fixture = LibraryControllerFixture{};
    auto const trackId = fixture.addTrack("Needle");

    auto controller = fixture.makeController();

    REQUIRE(controller.activeViewId() != rt::kInvalidViewId);
    auto const workspace = fixture.runtime().workspace().snapshot();
    REQUIRE(workspace.openViews.size() == 1);
    CHECK(workspace.activeViewId == controller.activeViewId());
    CHECK(fixture.runtime().views().trackListState(controller.activeViewId()).listId == rt::kAllTracksListId);
    CHECK_FALSE(fixture.runtime().workspace().canGoBack());
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks()[0].id == trackId);

    controller.setFilterDraft("Needle");
    CHECK(requireAppliedFilter(controller) == "Quick filter matched 1 track");
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks()[0].id == trackId);
  }

  TEST_CASE("LibraryController - sorting displayed track numbers keeps discs, grouping, filter and marked tracks")
  {
    auto fixture = LibraryControllerFixture{};
    auto const secondDisc = fixture.addTrack({.title = "Keep A", .discNumber = 2, .trackNumber = 1});
    auto const thirdTrack = fixture.addTrack({.title = "Keep B", .discNumber = 1, .trackNumber = 3});
    auto const secondTrack = fixture.addTrack({.title = "Keep C", .discNumber = 1, .trackNumber = 2});
    fixture.addTrack("Omit");
    fixture.listPresentations.setPresentationIdForList(rt::kAllTracksListId, rt::BuiltinPreset::Albums);
    auto controller = fixture.makeController();
    auto const viewId = controller.activeViewId();
    auto& views = fixture.runtime().views();
    controller.setFilterDraft("Keep");
    requireAppliedFilter(controller);
    auto const before = views.trackListState(viewId);
    REQUIRE(controller.tracks().size() == 3);
    controller.toggleVisualSelection();
    controller.moveTrackSelection(1);
    REQUIRE(controller.selectedTrackView().track->id == thirdTrack);

    REQUIRE(controller.sortByColumn("display-track-number"));
    CHECK(controller.tracks()[0].id == secondTrack);
    CHECK(controller.tracks()[1].id == thirdTrack);
    CHECK(controller.tracks()[2].id == secondDisc);
    REQUIRE(controller.sortByColumn("display-track-number"));
    auto const after = views.trackListState(viewId);
    CHECK(after.presentation.isPreset(rt::BuiltinPreset::Albums));
    CHECK(after.presentation.optColumnSort ==
          rt::TrackColumnSort{.column = rt::TrackField::DisplayTrackNumber, .ascending = false});
    CHECK(controller.tracks()[0].id == secondDisc);
    CHECK(controller.tracks()[1].id == thirdTrack);
    CHECK(controller.tracks()[2].id == secondTrack);
    CHECK(after.presentation.groupBy == before.presentation.groupBy);
    CHECK(after.presentation.visibleFields == before.presentation.visibleFields);
    CHECK(after.filterExpression == before.filterExpression);
    CHECK(controller.filterDraft() == "Keep");
    CHECK(controller.selectedTrackView().track->id == thirdTrack);
    CHECK(controller.markedIds() == std::unordered_set<TrackId>{secondTrack, thirdTrack});
    CHECK_FALSE(controller.isVisualSelectionActive());
    CHECK(fixture.listPresentations.presentationIdForList(rt::kAllTracksListId) ==
          rt::TrackPresetId{rt::BuiltinPreset::Albums});
  }

  TEST_CASE("LibraryController - changing sort columns replaces the override and reselecting Songs clears it")
  {
    auto fixture = LibraryControllerFixture{};
    auto const alpha = fixture.addTrack({.title = "Alpha", .artist = "Middle"});
    auto const bravo = fixture.addTrack({.title = "Bravo",
                                         .artist = "Middle",
                                         .uri = audio::test::requireAudioFixture("vbr_no_seek_table.mp3").string()});
    auto const charlie = fixture.addTrack({.title = "Charlie", .artist = "Early"});
    fixture.listPresentations.setPresentationIdForList(rt::kAllTracksListId, rt::BuiltinPreset::Songs);
    auto controller = fixture.makeController();
    auto const viewId = controller.activeViewId();
    auto const preset = controller.activePresentation();
    auto const selectedPreset = controller.selectedPresentation();
    REQUIRE(preset.isPreset(rt::BuiltinPreset::Songs));
    REQUIRE_FALSE(preset.optColumnSort);
    auto const trackIds = [&controller]
    {
      auto ids = std::vector<TrackId>{};

      for (auto const& entry : controller.tracks())
      {
        ids.push_back(entry.id);
      }

      return ids;
    };
    REQUIRE(trackIds() == std::vector{alpha, bravo, charlie});

    REQUIRE(controller.sortByColumn("duration"));
    CHECK(trackIds() == std::vector{alpha, charlie, bravo});
    REQUIRE(controller.sortByColumn("duration"));
    CHECK(trackIds() == std::vector{bravo, alpha, charlie});
    REQUIRE(controller.sortByColumn("artist"));
    // The tied artists follow preset title order, not the previous duration order.
    CHECK(trackIds() == std::vector{charlie, alpha, bravo});
    CHECK(controller.activePresentation().optColumnSort == rt::TrackColumnSort{.column = rt::TrackField::Artist});
    CHECK(controller.activePresentation().sortBy == preset.sortBy);
    CHECK(controller.activePresentation().isPreset(rt::BuiltinPreset::Songs));
    CHECK(controller.selectedPresentation() == selectedPreset);

    REQUIRE(controller.selectSelectedPresentation());
    CHECK(controller.activeViewId() == viewId);
    CHECK(controller.activePresentation() == preset);
    CHECK(trackIds() == std::vector{alpha, bravo, charlie});
  }

  TEST_CASE("LibraryController - equivalent column sort preserves an active visual range")
  {
    auto fixture = LibraryControllerFixture{};
    auto const first = fixture.addTrack("Alpha");
    auto const second = fixture.addTrack("Bravo");
    auto const third = fixture.addTrack("Charlie");
    fixture.listPresentations.setPresentationIdForList(rt::kAllTracksListId, rt::BuiltinPreset::Songs);
    auto controller = fixture.makeController();
    REQUIRE(controller.tracks().size() == 3);
    REQUIRE_FALSE(controller.activePresentation().optColumnSort);
    REQUIRE(controller.tracks()[0].id == first);
    REQUIRE(controller.tracks()[1].id == second);
    REQUIRE(controller.tracks()[2].id == third);
    controller.toggleFocusedMark();
    controller.setSelectedTrackIndex(1);
    controller.toggleVisualSelection();
    controller.moveTrackSelection(1);
    REQUIRE(controller.markedIds() == std::unordered_set<TrackId>{first, second, third});
    auto const revision = controller.trackRowsRevision();

    REQUIRE(controller.sortByColumn("title"));

    CHECK(controller.activePresentation().optColumnSort == rt::TrackColumnSort{.column = rt::TrackField::Title});
    CHECK(controller.isVisualSelectionActive());
    CHECK(controller.selectedTrackView().track->id == third);
    CHECK(controller.markedIds() == std::unordered_set<TrackId>{first, second, third});
    CHECK(controller.trackRowsRevision() == revision);
    controller.cancelVisualSelection();
    CHECK_FALSE(controller.isVisualSelectionActive());
    CHECK(controller.markedIds() == std::unordered_set<TrackId>{first});
  }

  TEST_CASE("LibraryController - Back and Forward preserve a column sort without a separate history stop")
  {
    auto fixture = LibraryControllerFixture{};
    auto const listId = fixture.addList("Saved");
    auto controller = fixture.makeController();
    auto const allTracksViewId = controller.activeViewId();
    REQUIRE(controller.openList(listId));
    auto const savedViewId = controller.activeViewId();
    auto const savedPresentation = controller.activePresentation();
    REQUIRE(controller.navigateHistory(false));
    REQUIRE(controller.activeViewId() == allTracksViewId);

    REQUIRE(controller.sortByColumn("title"));
    auto const sorted = controller.activePresentation();
    REQUIRE(sorted.optColumnSort == rt::TrackColumnSort{.column = rt::TrackField::Title});
    CHECK_FALSE(controller.canNavigateBack());
    REQUIRE(controller.canNavigateForward());
    REQUIRE(controller.navigateHistory(true));
    CHECK(controller.activeViewId() == savedViewId);
    CHECK(controller.activePresentation() == savedPresentation);
    REQUIRE(controller.navigateHistory(false));
    CHECK(controller.activeViewId() == allTracksViewId);
    CHECK(controller.activePresentation() == sorted);
    CHECK_FALSE(controller.canNavigateBack());
  }

  TEST_CASE("LibraryController - fallback navigation uses the shared presentation recommendation")
  {
    auto fixture = LibraryControllerFixture{};
    auto catalog = uimodel::TrackPresentationCatalog{fixture.runtime().workspace(), ao::test::englishMessageCatalog()};
    auto presentations = uimodel::ListPresentations{catalog, fixture.runtime().library().changes()};

    auto controller = LibraryController{fixture.runtime().library(),
                                        fixture.runtime().views(),
                                        fixture.runtime().workspace(),
                                        ao::test::englishMessageCatalog(),
                                        presentations,
                                        fixture.columnLayouts};

    CHECK(controller.activePresentation().isPreset(rt::BuiltinPreset::Albums));
    CHECK(presentations.snapshot().empty());
  }

  TEST_CASE("LibraryController - new list navigation uses the persisted presentation preference")
  {
    auto fixture = LibraryControllerFixture{};
    auto const listId = fixture.addList("Albums");
    auto catalog = uimodel::TrackPresentationCatalog{fixture.runtime().workspace(), ao::test::englishMessageCatalog()};
    auto presentations = uimodel::ListPresentations{catalog, fixture.runtime().library().changes()};
    presentations.setPresentationIdForList(rt::kAllTracksListId, rt::BuiltinPreset::Artists);
    presentations.setPresentationIdForList(listId, rt::BuiltinPreset::Albums);

    auto controller = LibraryController{fixture.runtime().library(),
                                        fixture.runtime().views(),
                                        fixture.runtime().workspace(),
                                        ao::test::englishMessageCatalog(),
                                        presentations,
                                        fixture.columnLayouts};

    CHECK(controller.activePresentation().isPreset(rt::BuiltinPreset::Artists));
    controller.navigation().reveal(listId);
    REQUIRE(controller.openList(controller.navigation().cursor()));
    CHECK(controller.currentListId() == listId);
    CHECK(controller.activePresentation().isPreset(rt::BuiltinPreset::Albums));
  }

  TEST_CASE("LibraryController - successful presentation selection records the list preference")
  {
    auto fixture = LibraryControllerFixture{};
    auto catalog = uimodel::TrackPresentationCatalog{fixture.runtime().workspace(), ao::test::englishMessageCatalog()};
    auto presentations = uimodel::ListPresentations{catalog, fixture.runtime().library().changes()};
    auto controller = LibraryController{fixture.runtime().library(),
                                        fixture.runtime().views(),
                                        fixture.runtime().workspace(),
                                        ao::test::englishMessageCatalog(),
                                        presentations,
                                        fixture.columnLayouts};

    CHECK(controller.setPresentation(rt::BuiltinPreset::Albums) == "View: albums");

    REQUIRE(presentations.snapshot().contains(rt::kAllTracksListId));
    CHECK(presentations.snapshot().at(rt::kAllTracksListId) == rt::TrackPresetId{rt::BuiltinPreset::Albums});
  }

  TEST_CASE("LibraryController - failed presentation selection does not change the list preference")
  {
    auto fixture = LibraryControllerFixture{};
    auto catalog = uimodel::TrackPresentationCatalog{fixture.runtime().workspace(), ao::test::englishMessageCatalog()};
    auto presentations = uimodel::ListPresentations{catalog, fixture.runtime().library().changes()};
    presentations.setPresentationIdForList(rt::kAllTracksListId, rt::BuiltinPreset::Albums);
    auto controller = LibraryController{fixture.runtime().library(),
                                        fixture.runtime().views(),
                                        fixture.runtime().workspace(),
                                        ao::test::englishMessageCatalog(),
                                        presentations,
                                        fixture.columnLayouts};
    auto const activeBefore = controller.activePresentation().presetId;

    auto const missingRes = controller.setPresentation(rt::CustomPresetId{77});
    REQUIRE_FALSE(missingRes);
    CHECK(missingRes.error().message == "Unknown view 000000000000004d");

    CHECK(controller.activePresentation().presetId == activeBefore);
    CHECK(presentations.snapshot().at(rt::kAllTracksListId) == rt::TrackPresetId{rt::BuiltinPreset::Albums});
  }

  TEST_CASE("LibraryController - a stale presentation choice cannot change either view or pin")
  {
    auto fixture = LibraryControllerFixture{};
    auto const listId = fixture.addList("Saved");
    fixture.listPresentations.setPresentationIdForList(rt::kAllTracksListId, rt::BuiltinPreset::Artists);
    fixture.listPresentations.setPresentationIdForList(listId, rt::BuiltinPreset::Songs);
    auto controller = fixture.makeController();
    auto const textCatalog = ao::test::messageCatalog("zh-Hans");
    controller.setTextCatalog(textCatalog);
    auto& workspace = fixture.runtime().workspace();
    auto& views = fixture.runtime().views();
    auto const allTracksViewId = controller.activeViewId();
    REQUIRE(controller.openList(listId));
    auto const savedViewId = controller.activeViewId();

    // Another workspace owner replayed All Tracks without reattaching the TUI.
    REQUIRE(workspace.goBack());
    auto const workspaceBefore = workspace.snapshot();
    REQUIRE(workspaceBefore.activeViewId == allTracksViewId);
    REQUIRE(controller.activeViewId() == savedViewId);
    REQUIRE(controller.currentListId() == listId);
    REQUIRE(workspace.canGoForward());
    auto const allTracksBefore = views.trackListState(allTracksViewId).presentation;
    auto const savedBefore = views.trackListState(savedViewId).presentation;
    auto const pinsBefore = fixture.listPresentations.snapshot();

    auto const staleRes = controller.setPresentation(rt::BuiltinPreset::ListOrder);
    REQUIRE_FALSE(staleRes);
    CHECK(staleRes.error().severity == rt::NotificationSeverity::Warning);
    CHECK(staleRes.error().message ==
          i18n::requiredText(textCatalog, i18n::MessageId::TuiLibraryPresentationViewChanged));

    CHECK(controller.activeViewId() == savedViewId);
    CHECK(controller.currentListId() == listId);
    CHECK(views.trackListState(allTracksViewId).presentation == allTracksBefore);
    CHECK(views.trackListState(savedViewId).presentation == savedBefore);
    CHECK(fixture.listPresentations.snapshot() == pinsBefore);
    CHECK(workspace.snapshot() == workspaceBefore);
    REQUIRE(workspace.goForward());
    CHECK(workspace.snapshot().activeViewId == savedViewId);
    CHECK(views.trackListState(savedViewId).presentation == savedBefore);
    REQUIRE(workspace.goBack());

    auto const reopenedRes = controller.openList(listId);
    REQUIRE(reopenedRes);
    REQUIRE(*reopenedRes);
    CHECK(workspace.snapshot().activeViewId == savedViewId);
    REQUIRE(controller.setPresentation(rt::BuiltinPreset::ListOrder));
    CHECK(views.trackListState(savedViewId).presentation.isPreset(rt::BuiltinPreset::ListOrder));
    CHECK(fixture.listPresentations.presentationIdForList(listId) == rt::TrackPresetId{rt::BuiltinPreset::ListOrder});
    CHECK(views.trackListState(allTracksViewId).presentation == allTracksBefore);
    CHECK(fixture.listPresentations.presentationIdForList(rt::kAllTracksListId) ==
          rt::TrackPresetId{rt::BuiltinPreset::Artists});
  }

  TEST_CASE("LibraryController - persisted preference does not replace an exact restored active view")
  {
    auto fixture = LibraryControllerFixture{};
    auto const& albums = rt::builtinTrackPresentationPreset(rt::BuiltinPreset::Albums);
    auto const activeViewId = ao::test::requireValue(fixture.runtime().workspace().navigate({
      .target = rt::GlobalViewKind::AllTracks,
      .optPresentation =
        rt::NavigationPresentation{
          .mode = rt::NavigationPresentationMode::Override,
          .spec = albums.spec,
        },
    }));
    auto const workspaceBefore = fixture.runtime().workspace().snapshot();
    auto catalog = uimodel::TrackPresentationCatalog{fixture.runtime().workspace(), ao::test::englishMessageCatalog()};
    auto presentations = uimodel::ListPresentations{catalog, fixture.runtime().library().changes()};
    presentations.setPresentationIdForList(rt::kAllTracksListId, rt::BuiltinPreset::Artists);

    auto controller = LibraryController{fixture.runtime().library(),
                                        fixture.runtime().views(),
                                        fixture.runtime().workspace(),
                                        ao::test::englishMessageCatalog(),
                                        presentations,
                                        fixture.columnLayouts};

    CHECK(controller.activeViewId() == activeViewId);
    CHECK(controller.activePresentation().isPreset(rt::BuiltinPreset::Albums));
    CHECK(fixture.runtime().workspace().snapshot() == workspaceBefore);
  }

  TEST_CASE("LibraryController - construction attaches the exact active workspace view")
  {
    auto tempDir = ao::test::TempDir{};
    auto firstTrackId = kInvalidTrackId;
    auto secondTrackId = kInvalidTrackId;

    {
      auto runtimeOwnerPtr = rt::test::makeStateOnlyRuntime(tempDir);
      auto& runtime = runtimeOwnerPtr->runtime();
      firstTrackId = rt::test::addRuntimeTrack(*runtimeOwnerPtr, library::test::TrackSpec{.title = "First"});
      secondTrackId = rt::test::addRuntimeTrack(*runtimeOwnerPtr, library::test::TrackSpec{.title = "Second"});
      auto const& songs = rt::builtinTrackPresentationPreset(rt::BuiltinPreset::Songs);
      auto const& albums = rt::builtinTrackPresentationPreset(rt::BuiltinPreset::Albums);
      REQUIRE(runtime.workspace().navigate({
        .target =
          rt::FilteredListTarget{
            .listId = rt::kAllTracksListId,
            .filterExpression = "$title ~ \"First\"",
          },
        .optPresentation =
          rt::NavigationPresentation{
            .mode = rt::NavigationPresentationMode::Override,
            .spec = songs.spec,
          },
      }));
      REQUIRE(runtime.workspace().navigate({
        .target =
          rt::FilteredListTarget{
            .listId = rt::kAllTracksListId,
            .filterExpression = "$title ~ \"Second\"",
          },
        .optPresentation =
          rt::NavigationPresentation{
            .mode = rt::NavigationPresentationMode::Override,
            .spec = albums.spec,
          },
      }));
      runtime.workspace().saveSession(runtime.workspaceConfigStore());
    }

    auto runtimeOwnerPtr = rt::test::makeStateOnlyRuntime(tempDir);
    auto& runtime = runtimeOwnerPtr->runtime();
    REQUIRE(runtime.workspace().restoreSession(runtime.workspaceConfigStore()));
    rt::test::settleRuntimeCallbacks(runtime);
    auto const before = runtime.workspace().snapshot();
    REQUIRE(before.openViews.size() == 2);
    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto const expectedActiveViewId = before.activeViewId;
    auto const expectedState = runtime.views().trackListState(expectedActiveViewId);
    REQUIRE(expectedState.filterExpression == "$title ~ \"Second\"");

    auto presentationCatalog =
      uimodel::TrackPresentationCatalog{runtime.workspace(), ao::test::englishMessageCatalog()};
    auto listPresentations = uimodel::ListPresentations{presentationCatalog, runtime.library().changes()};
    auto controller = LibraryController{runtime.library(),
                                        runtime.views(),
                                        runtime.workspace(),
                                        ao::test::englishMessageCatalog(),
                                        listPresentations,
                                        columnLayouts};

    CHECK(controller.activeViewId() == expectedActiveViewId);
    CHECK(runtime.workspace().snapshot() == before);
    CHECK(controller.currentListId() == rt::kAllTracksListId);
    CHECK(controller.navigation().cursor() == rt::kAllTracksListId);
    CHECK(controller.filterDraft() == expectedState.filterExpression);
    CHECK(controller.activePresentation().isPreset(rt::BuiltinPreset::Albums));
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks()[0].id == secondTrackId);
    CHECK(controller.tracks()[0].id != firstTrackId);
    auto const selection = runtime.views().trackListState(expectedActiveViewId).selection;
    REQUIRE(selection.size() == 1);
    CHECK(selection[0] == secondTrackId);
  }

  TEST_CASE("LibraryController - selecting a preset refreshes visible paths and keeps untitled filenames")
  {
    auto fixture = LibraryControllerFixture{};
    auto const titledId = fixture.addTrack("Tagged title");
    auto const untitledId = fixture.addTrack("");
    auto const optExpectedTitled = fixture.runtime().library().snapshot().trackRow(titledId);
    REQUIRE(optExpectedTitled);
    REQUIRE(optExpectedTitled->optUriPath);
    auto const optExpectedUntitled = fixture.runtime().library().snapshot().trackRow(untitledId);
    REQUIRE(optExpectedUntitled);
    REQUIRE(optExpectedUntitled->title.empty());
    REQUIRE(optExpectedUntitled->optUriPath);
    auto const fileName = utility::pathToUtf8(optExpectedUntitled->optUriPath->filename());
    REQUIRE_FALSE(fileName.empty());

    auto controller = fixture.makeController();
    auto const viewId = controller.activeViewId();
    auto const verifyPaths = [&](bool const filePathVisible)
    {
      auto const titled = std::ranges::find(controller.tracks(), titledId, &TrackListEntry::id);
      auto const untitled = std::ranges::find(controller.tracks(), untitledId, &TrackListEntry::id);
      REQUIRE(titled != controller.tracks().end());
      REQUIRE(untitled != controller.tracks().end());
      CHECK(titled->row.optUriPath == (filePathVisible ? optExpectedTitled->optUriPath : std::nullopt));
      CHECK(untitled->row.optUriPath == optExpectedUntitled->optUriPath);
      CHECK(trackDisplayTitle(ao::test::englishMessageCatalog(), untitled->row) == fileName);
    };
    REQUIRE(controller.activePresentation().isPreset(rt::BuiltinPreset::Albums));
    verifyPaths(false);

    auto const pathId = rt::CustomPresetId{1};
    auto pathSpec = rt::defaultTrackPresentationSpec();
    pathSpec.presetId = pathId;
    pathSpec.visibleFields = {rt::TrackField::Title, rt::TrackField::FilePath};
    REQUIRE(fixture.runtime().workspace().addCustomPreset(
      {.label = "With File Path", .optBasePresetId = rt::BuiltinPreset::Songs, .spec = pathSpec}));
    REQUIRE(controller.setPresentation(pathId));
    CHECK(controller.activeViewId() == viewId);
    verifyPaths(true);

    auto const artistId = rt::CustomPresetId{2};
    auto customSpec = rt::defaultTrackPresentationSpec();
    customSpec.presetId = artistId;
    customSpec.visibleFields = {rt::TrackField::Artist};
    REQUIRE(fixture.runtime().workspace().addCustomPreset(
      {.label = "Artist Only", .optBasePresetId = rt::BuiltinPreset::Songs, .spec = customSpec}));
    REQUIRE(controller.setPresentation(artistId));
    CHECK(controller.activePresentation().visibleFields == std::vector{rt::TrackField::Artist});
    verifyPaths(false);
    CHECK(controller.reloadActiveList() == "Reloaded 2 tracks");
    verifyPaths(false);
  }

  TEST_CASE("LibraryController - navigation loads paths according to the destination list's columns")
  {
    auto fixture = LibraryControllerFixture{};
    auto const titledId = fixture.addTrack({.title = "Destination track", .artist = "Matched Artist"});
    auto const untitledId = fixture.addTrack({.title = "", .artist = "Matched Artist"});
    auto const addList = [&](std::string name)
    {
      return ao::test::requireValue(rt::test::runRuntimeTask(fixture.runtime(),
                                                             fixture.runtimeOwnerPtr->libraryCommands().createListAsync(
                                                               {.name = std::move(name), .expression = "$artist?"})));
    };
    auto const hiddenListId = addList("Hidden path");
    auto const visibleListId = addList("Visible path");
    fixture.listPresentations.setPresentationIdForList(hiddenListId, rt::BuiltinPreset::Technical);
    fixture.listPresentations.setPresentationIdForList(visibleListId, rt::BuiltinPreset::Technical);
    fixture.columnLayouts.updateLayout(
      hiddenListId, {{.field = rt::TrackField::FilePath, .visibility = uimodel::TrackColumnVisibility::Hide}});
    fixture.columnLayouts.updateLayout(visibleListId, {{.field = rt::TrackField::FilePath, .weight = 1.0}});

    auto const optExpectedTitled = fixture.runtime().library().snapshot().trackRow(titledId);
    auto const optExpectedUntitled = fixture.runtime().library().snapshot().trackRow(untitledId);
    REQUIRE(optExpectedTitled);
    REQUIRE(optExpectedTitled->optUriPath);
    REQUIRE(optExpectedUntitled);
    REQUIRE(optExpectedUntitled->optUriPath);
    auto controller = fixture.makeController();
    auto const verifyDestination = [&](ListId const listId, bool const filePathVisible)
    {
      CHECK(controller.currentListId() == listId);
      CHECK(controller.activePresentation().isPreset(rt::BuiltinPreset::Technical));
      auto const titled = std::ranges::find(controller.tracks(), titledId, &TrackListEntry::id);
      auto const untitled = std::ranges::find(controller.tracks(), untitledId, &TrackListEntry::id);
      REQUIRE(titled != controller.tracks().end());
      REQUIRE(untitled != controller.tracks().end());
      CHECK(titled->row.optUriPath == (filePathVisible ? optExpectedTitled->optUriPath : std::nullopt));
      CHECK(untitled->row.optUriPath == optExpectedUntitled->optUriPath);
    };

    REQUIRE(ao::test::requireValue(controller.openList(hiddenListId)));
    verifyDestination(hiddenListId, false);
    CHECK(controller.reloadActiveList() == "Reloaded 2 tracks");
    verifyDestination(hiddenListId, false);

    REQUIRE(ao::test::requireValue(controller.openList(visibleListId)));
    verifyDestination(visibleListId, true);
    REQUIRE(controller.navigateHistory(false));
    verifyDestination(hiddenListId, false);
    REQUIRE(controller.navigateHistory(true));
    verifyDestination(visibleListId, true);
  }

  TEST_CASE("LibraryController - construction keeps a valid empty restored view")
  {
    auto tempDir = ao::test::TempDir{};

    {
      auto runtimeOwnerPtr = rt::test::makeStateOnlyRuntime(tempDir);
      auto& runtime = runtimeOwnerPtr->runtime();
      REQUIRE(rt::test::addRuntimeTrack(*runtimeOwnerPtr, library::test::TrackSpec{.title = "Present"}) !=
              kInvalidTrackId);
      REQUIRE(runtime.workspace().navigate({
        .target =
          rt::FilteredListTarget{
            .listId = rt::kAllTracksListId,
            .filterExpression = "$title ~ \"Absent\"",
          },
      }));
      runtime.workspace().saveSession(runtime.workspaceConfigStore());
    }

    auto runtimeOwnerPtr = rt::test::makeStateOnlyRuntime(tempDir);
    auto& runtime = runtimeOwnerPtr->runtime();
    REQUIRE(runtime.workspace().restoreSession(runtime.workspaceConfigStore()));
    rt::test::settleRuntimeCallbacks(runtime);
    auto const before = runtime.workspace().snapshot();
    REQUIRE(before.openViews.size() == 1);
    auto const expectedActiveViewId = before.activeViewId;

    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto presentationCatalog =
      uimodel::TrackPresentationCatalog{runtime.workspace(), ao::test::englishMessageCatalog()};
    auto listPresentations = uimodel::ListPresentations{presentationCatalog, runtime.library().changes()};
    auto controller = LibraryController{runtime.library(),
                                        runtime.views(),
                                        runtime.workspace(),
                                        ao::test::englishMessageCatalog(),
                                        listPresentations,
                                        columnLayouts};

    CHECK(controller.activeViewId() == expectedActiveViewId);
    CHECK(runtime.workspace().snapshot() == before);
    CHECK(controller.filterDraft() == "$title ~ \"Absent\"");
    CHECK(controller.tracks().empty());
  }

  TEST_CASE("LibraryController - reload preserves the active view configuration")
  {
    auto fixture = LibraryControllerFixture{};
    auto const matchingTrackId = fixture.addTrack("Needle");
    fixture.addTrack("Other");

    auto controller = fixture.makeController();
    controller.setFilterDraft("Needle");
    CHECK(requireAppliedFilter(controller) == "Quick filter matched 1 track");
    CHECK(controller.setPresentation(rt::BuiltinPreset::Albums) == "View: albums");
    REQUIRE(controller.tracks().size() == 1);
    auto const activeViewId = controller.activeViewId();
    auto const workspaceBefore = fixture.runtime().workspace().snapshot();
    auto const viewBefore = fixture.runtime().views().trackListState(activeViewId);

    CHECK(controller.reloadActiveList() == "Reloaded 1 track");

    CHECK(controller.activeViewId() == activeViewId);
    CHECK(fixture.runtime().workspace().snapshot() == workspaceBefore);
    CHECK(fixture.runtime().views().trackListState(activeViewId).filterExpression == viewBefore.filterExpression);
    CHECK(fixture.runtime().views().trackListState(activeViewId).presentation == viewBefore.presentation);
    CHECK(controller.filterDraft() == "Needle");
    CHECK(controller.activePresentation().isPreset(rt::BuiltinPreset::Albums));
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks()[0].id == matchingTrackId);
  }

  TEST_CASE("LibraryController - track-only reload preserves chooser highlights")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("Initial");
    auto const otherListId = fixture.addList("Other");
    auto controller = fixture.makeController();
    controller.navigation().reveal(otherListId);
    REQUIRE(controller.presentationEntries().size() > 1);
    auto const activePreset = controller.activePresentation().presetId;
    auto const selectedPresentation = controller.selectedPresentation() == 0 ? 1 : 0;
    REQUIRE(controller.trySetSelectedPresentation(selectedPresentation));

    fixture.addTrack("Added");
    rt::test::settleRuntimeCallbacks(fixture.runtime());

    CHECK(controller.navigation().cursor() == otherListId);
    CHECK(controller.selectedPresentation() == selectedPresentation);
    CHECK(controller.currentListId() == rt::kAllTracksListId);
    CHECK(controller.activePresentation().presetId == activePreset);
    CHECK(controller.tracks().size() == 2);
  }

  TEST_CASE("LibraryController - navigation refresh localizes unnamed Lists for both consumers")
  {
    auto fixture = LibraryControllerFixture{};
    auto const unnamedId = fixture.addList("");

    auto controller = LibraryController{fixture.runtime().library(),
                                        fixture.runtime().views(),
                                        fixture.runtime().workspace(),
                                        ao::test::messageCatalog("de"),
                                        fixture.listPresentations,
                                        fixture.columnLayouts};

    auto const chooserEntry = std::ranges::find(controller.libraryEntries(), unnamedId, &LibraryNavEntry::id);
    REQUIRE(chooserEntry != controller.libraryEntries().end());
    CHECK(chooserEntry->label == "[L] <Unbenannte Liste>");

    auto const treeRow = std::ranges::find(controller.navigation().rows(), unnamedId, &ListNavigationRow::id);
    REQUIRE(treeRow != controller.navigation().rows().end());
    CHECK(treeRow->name == "<Unbenannte Liste>");
  }

  TEST_CASE("LibraryController - chooser selection clamps and can reveal the active List")
  {
    auto fixture = LibraryControllerFixture{};
    auto const firstId = fixture.addList("First");
    auto const lastId = fixture.addList("Last");
    auto controller = fixture.makeController();

    controller.selectListRow(-10);
    CHECK(controller.selectedList() == 0);
    CHECK(controller.libraryEntries()[controller.selectedList()].id == rt::kAllTracksListId);
    controller.selectListRow(100);
    CHECK(controller.selectedList() == static_cast<std::int32_t>(controller.libraryEntries().size()) - 1);
    CHECK(controller.libraryEntries()[controller.selectedList()].id == lastId);

    REQUIRE(controller.openList(firstId));
    controller.selectListRow(100);
    controller.revealActiveList();
    CHECK(controller.libraryEntries()[controller.selectedList()].id == firstId);
  }

  TEST_CASE("LibraryController - chooser refresh retains the selected List identity when rows shift")
  {
    auto fixture = LibraryControllerFixture{};
    auto const removedId = fixture.addList("Removed before selection");
    auto const selectedId = fixture.addList("Retained selection");
    auto controller = fixture.makeController();
    auto const selected = std::ranges::find(controller.libraryEntries(), selectedId, &LibraryNavEntry::id);
    REQUIRE(selected != controller.libraryEntries().end());
    controller.selectListRow(static_cast<std::int32_t>(selected - controller.libraryEntries().begin()));
    REQUIRE(controller.libraryEntries()[controller.selectedList()].id == selectedId);

    REQUIRE(rt::test::runRuntimeTask(
      fixture.runtime(), fixture.runtimeOwnerPtr->libraryCommands().deleteListAsync(removedId)));
    rt::test::settleRuntimeCallbacks(fixture.runtime());

    CHECK(controller.libraryEntries()[controller.selectedList()].id == selectedId);
    CHECK(controller.selectedList() == 1);
  }

  TEST_CASE("LibraryController - reload falls back when the active view disappeared")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("Needle");
    fixture.addTrack("Other");
    auto controller = fixture.makeController();
    controller.setFilterDraft("Needle");
    REQUIRE(controller.applyFilter());
    auto const closedViewId = controller.activeViewId();
    REQUIRE(fixture.runtime().workspace().closeView(closedViewId));

    CHECK(controller.reloadActiveList() == "Reloaded 2 tracks");

    CHECK(controller.activeViewId() != closedViewId);
    CHECK(controller.currentListId() == rt::kAllTracksListId);
    CHECK(controller.filterDraft().empty());
    REQUIRE(controller.tracks().size() == 2);
    CHECK(fixture.runtime().workspace().snapshot().activeViewId == controller.activeViewId());
  }

  TEST_CASE("LibraryController - list deletion recovers without observer-order assumptions")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("Needle");
    auto const temporaryListId = fixture.addList("Temporary");
    auto catalog = uimodel::TrackPresentationCatalog{fixture.runtime().workspace(), ao::test::englishMessageCatalog()};
    auto presentations = uimodel::ListPresentations{catalog, fixture.runtime().library().changes()};
    presentations.setPresentationIdForList(temporaryListId, rt::BuiltinPreset::Albums);
    fixture.columnLayouts.updateLayout(
      temporaryListId, {uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 17, .weight = -1.0}});
    auto controller = LibraryController{fixture.runtime().library(),
                                        fixture.runtime().views(),
                                        fixture.runtime().workspace(),
                                        ao::test::englishMessageCatalog(),
                                        presentations,
                                        fixture.columnLayouts};
    auto const allTracksViewId = controller.activeViewId();
    controller.navigation().reveal(temporaryListId);
    REQUIRE(controller.openList(controller.navigation().cursor()));
    REQUIRE(controller.currentListId() == temporaryListId);
    REQUIRE(fixture.columnLayouts.snapshot().contains(temporaryListId));

    REQUIRE(rt::test::runRuntimeTask(
      fixture.runtime(), fixture.runtimeOwnerPtr->libraryCommands().deleteListAsync(temporaryListId)));
    rt::test::settleRuntimeCallbacks(fixture.runtime());

    CHECK(controller.activeViewId() == allTracksViewId);
    CHECK(controller.currentListId() == rt::kAllTracksListId);
    CHECK(fixture.runtime().workspace().snapshot().activeViewId == allTracksViewId);
    CHECK_FALSE(presentations.snapshot().contains(temporaryListId));
    CHECK_FALSE(fixture.columnLayouts.snapshot().contains(temporaryListId));
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks()[0].row.title == "Needle");
  }

  TEST_CASE("LibraryController - expression filters report expression mode")
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

  TEST_CASE("LibraryController - exposes and clears transient expression errors")
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

  TEST_CASE("LibraryController - opening another list clears the previous view filter error")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("Needle");
    auto const cleanListId = fixture.addList("Clean");

    auto controller = fixture.makeController();
    controller.setFilterDraft("$artist =");
    REQUIRE(controller.applyFilter());
    REQUIRE(controller.filterError().contains("Filter error:"));

    controller.navigation().reveal(cleanListId);

    auto const openedRes = controller.openList(controller.navigation().cursor());

    REQUIRE(openedRes);
    CHECK(controller.currentListId() == cleanListId);
    CHECK(controller.filterDraft().empty());
    CHECK(controller.filterError().empty());
  }

  TEST_CASE("LibraryController - filter error preserves visible controller state")
  {
    auto fixture = LibraryControllerFixture{};
    auto const firstTrackId = fixture.addTrack("Needle");
    fixture.addTrack("Other");

    auto controller = fixture.makeController();
    auto const activeViewId = controller.activeViewId();
    auto const sectionCount = controller.sections().size();
    controller.toggleFocusedMark();
    controller.moveTrackSelection(1);
    auto const selectedTrack = controller.selectedTrack();
    REQUIRE(selectedTrack == 1);
    REQUIRE(controller.markedIds().contains(firstTrackId));
    controller.setFilterDraft("Needle");
    REQUIRE(fixture.runtime().workspace().closeView(activeViewId));

    auto res = controller.applyFilter();

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotFound);
    CHECK(controller.filterError().contains("Filter error:"));
    CHECK(controller.filterDraft() == "Needle");
    CHECK(controller.activeViewId() == activeViewId);
    CHECK(controller.selectedTrack() == selectedTrack);
    CHECK(controller.sections().size() == sectionCount);
    REQUIRE(controller.tracks().size() == 2);
    CHECK(controller.tracks().front().id == firstTrackId);
    CHECK(controller.activePresentation() == rt::defaultTrackPresentationSpec());
    CHECK(controller.markedIds() == std::unordered_set<TrackId>{firstTrackId});
  }

  TEST_CASE("LibraryController - empty track views clear the published selection")
  {
    auto fixture = LibraryControllerFixture{};
    auto const trackId = fixture.addTrack("Needle");

    auto controller = fixture.makeController();
    auto const activeViewId = controller.activeViewId();
    auto const selectionBefore = fixture.runtime().views().trackListState(activeViewId).selection;
    REQUIRE(selectionBefore.size() == 1);
    CHECK(selectionBefore.front() == trackId);
    controller.setFilterDraft("not-present");

    CHECK(requireAppliedFilter(controller) == "Quick filter matched 0 tracks");
    CHECK(controller.tracks().empty());
    CHECK(fixture.runtime().views().trackListState(activeViewId).selection.empty());
    CHECK(fixture.runtime().workspace().snapshot().activeViewId == activeViewId);

    auto selected = controller.selectedTrackView();
    CHECK(selected.track == nullptr);
    CHECK(selected.coverArtId == kInvalidResourceId);
  }

  TEST_CASE("LibraryController - revealTrack selects a visible track")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("First");
    auto const secondId = fixture.addTrack("Second");

    auto controller = fixture.makeController();

    CHECK(ao::test::requireValue(controller.revealTrack(secondId)) == "Revealed Second");
    CHECK(controller.selectedTrack() == 1);
    REQUIRE(controller.selectedTrackView().track != nullptr);
    CHECK(controller.selectedTrackView().track->id == secondId);
  }

  TEST_CASE("LibraryController - revealTrack opens an unfiltered view and preserves the previous filter")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("First");
    auto const hiddenId = fixture.addTrack("Hidden");

    auto controller = fixture.makeController();

    auto const invalidRes = controller.revealTrack(kInvalidTrackId);
    REQUIRE_FALSE(invalidRes);
    CHECK(invalidRes.error().message == "No current track");

    controller.setFilterDraft("First");
    CHECK(requireAppliedFilter(controller) == "Quick filter matched 1 track");
    auto const filteredView = controller.activeViewId();
    auto const expression = fixture.runtime().views().trackListState(filteredView).filterExpression;
    CHECK(ao::test::requireValue(controller.revealTrack(hiddenId)) == "Revealed Hidden");
    CHECK(controller.activeViewId() != filteredView);
    CHECK(controller.tracks()[controller.selectedTrack()].id == hiddenId);
    CHECK(fixture.runtime().views().trackListState(filteredView).filterExpression == expression);
    REQUIRE(controller.navigateHistory(false));
    CHECK(controller.activeViewId() == filteredView);
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks().front().row.title == "First");
    REQUIRE(controller.navigateHistory(true));
    CHECK(controller.tracks().size() == 2);
  }

  TEST_CASE("LibraryController - revealing a track leaves no extra Back stop after a column sort")
  {
    auto fixture = LibraryControllerFixture{};
    auto const trackA = fixture.addTrack({.title = "Only in A", .artist = "Artist A"});
    auto const otherB = fixture.addTrack({.title = "Earlier in B", .artist = "Artist B"});
    auto const targetB = fixture.addTrack({.title = "Target in B", .artist = "Artist B"});
    auto const outside = fixture.addTrack({.title = "Outside both views", .artist = "Artist C"});
    auto controller = fixture.makeController();
    auto& workspace = fixture.runtime().workspace();
    auto const home = controller.activeViewId();
    auto expectedHome = home;

    REQUIRE(controller.navigateToArtist("Artist A"));
    auto const viewA = controller.activeViewId();
    auto const filterA = std::string{controller.filterDraft()};
    REQUIRE(controller.navigateToArtist("Artist B"));
    auto const viewB = controller.activeViewId();
    auto const filterB = std::string{controller.filterDraft()};
    REQUIRE(controller.tracks().size() == 2);
    REQUIRE(controller.tracks().front().id == otherB);
    REQUIRE(controller.navigateHistory(false));
    REQUIRE(controller.activeViewId() == viewA);
    REQUIRE(controller.sortByColumn("title"));
    REQUIRE(controller.canNavigateForward());

    auto expectedTrack = outside;

    SECTION("The exact preferred filtered view wins over a plain view of the same List")
    {
      expectedTrack = targetB;
      REQUIRE(controller.revealTrack(targetB, viewB, rt::kAllTracksListId));
      CHECK(controller.activeViewId() == viewB);
      CHECK(controller.filterDraft() == filterB);
      CHECK(controller.tracks().size() == 2);
    }

    SECTION("A preferred view that excludes the track falls back to the open plain view")
    {
      REQUIRE(controller.revealTrack(outside, viewB));
      CHECK(controller.activeViewId() == home);
      CHECK(controller.filterDraft().empty());
    }

    SECTION("Fallback opens a plain view when no plain view is open")
    {
      REQUIRE(workspace.closeView(home));
      REQUIRE(controller.revealTrack(outside, viewB));
      expectedHome = controller.activeViewId();
      CHECK(expectedHome != home);
      CHECK(controller.filterDraft().empty());
      CHECK(controller.tracks().size() == 4);
    }

    auto const destination = controller.activeViewId();
    CHECK(workspace.snapshot().activeViewId == destination);
    auto const& views = fixture.runtime().views();
    CHECK(views.trackListState(viewB).filterExpression == filterB);
    CHECK(views.trackListState(destination).selection == std::vector{expectedTrack});
    auto const* selected = controller.selectedTrackView().track;
    REQUIRE(selected != nullptr);
    CHECK(selected->id == expectedTrack);
    CHECK_FALSE(controller.canNavigateForward());

    REQUIRE(controller.navigateHistory(false));
    CHECK(controller.activeViewId() == viewA);
    CHECK(controller.filterDraft() == filterA);
    CHECK(controller.activePresentation().optColumnSort == rt::TrackColumnSort{.column = rt::TrackField::Title});
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks().front().id == trackA);

    REQUIRE(controller.navigateHistory(false));
    CHECK(controller.activeViewId() == expectedHome);
    CHECK(controller.filterDraft().empty());
    CHECK_FALSE(controller.activePresentation().optColumnSort);
    CHECK_FALSE(controller.canNavigateBack());
  }

  TEST_CASE(
    "LibraryController - in-place playback reveal follows live rows without discarding the filter draft or marks")
  {
    auto fixture = LibraryControllerFixture{};
    auto const target = fixture.addTrack(library::test::TrackSpec{.title = "Alpha", .album = "Z"});
    auto const other = fixture.addTrack(library::test::TrackSpec{.title = "Beta", .album = "A"});
    auto controller = fixture.makeController();
    REQUIRE(controller.setPresentation(rt::BuiltinPreset::Songs));
    REQUIRE(controller.tracks().size() == 2);
    REQUIRE(controller.tracks().front().id == target);
    auto const viewId = controller.activeViewId();
    auto& views = fixture.runtime().views();
    std::int32_t expectedRow = 0;

    SECTION("A replaced projection makes a hidden track visible again")
    {
      controller.setFilterDraft("Beta");
      REQUIRE(controller.applyFilter());
      REQUIRE(controller.tracks().size() == 1);
      REQUIRE(controller.tracks().front().id == other);
      controller.toggleFocusedMark();
      REQUIRE(views.setFilter(viewId, ""));
    }

    SECTION("An in-place presentation change reorders and groups the still-visible target")
    {
      controller.setSelectedTrackIndex(1);
      controller.toggleFocusedMark();
      REQUIRE(fixture.runtime().workspace().setActivePresentation(rt::BuiltinPreset::Albums, {.recordHistory = false}));
      expectedRow = 1;
    }

    controller.setFilterDraft("unfinished");
    CHECK(ao::test::requireValue(controller.revealPlayingTrack(target, rt::kAllTracksListId)) == "Revealed Alpha");
    REQUIRE(controller.tracks().size() == 2);
    CHECK(controller.selectedTrack() == expectedRow);
    CHECK(controller.tracks()[controller.selectedTrack()].id == target);
    CHECK(controller.filterDraft() == "unfinished");
    CHECK(controller.markedIds() == std::unordered_set{other});
    CHECK(controller.activeViewId() == viewId);
  }

  TEST_CASE("LibraryController - revealTrack selects a track after presentation reorder")
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
    REQUIRE(controller.setPresentation(rt::BuiltinPreset::Albums) == "View: albums");
    REQUIRE(controller.tracks().size() == 3);
    CHECK(controller.tracks()[0].id == targetId);
    CHECK(controller.tracks()[1].id == secondId);
    CHECK(controller.tracks()[2].id == lastId);

    CHECK(ao::test::requireValue(controller.revealTrack(targetId)) == "Revealed A One");
    CHECK(controller.selectedTrack() == 0);
    REQUIRE(controller.selectedTrackView().track != nullptr);
    CHECK(controller.selectedTrackView().track->id == targetId);
  }

  TEST_CASE("LibraryController - setPresentation applies active workspace presentation")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("First");

    auto controller = fixture.makeController();

    CHECK(controller.setPresentation(rt::BuiltinPreset::Albums) == "View: albums");
    CHECK(fixture.runtime()
            .views()
            .trackListState(controller.activeViewId())
            .presentation.isPreset(rt::BuiltinPreset::Albums));
    auto const missingRes = controller.setPresentationKey("missing-preset");
    REQUIRE_FALSE(missingRes);
    CHECK(missingRes.error().message == "Unknown view missing-preset");
    CHECK(fixture.runtime()
            .views()
            .trackListState(controller.activeViewId())
            .presentation.isPreset(rt::BuiltinPreset::Albums));
  }

  TEST_CASE("LibraryController - construction reads custom presets already restored in workspace")
  {
    auto tempDir = ao::test::TempDir{};
    auto customSpec = rt::defaultTrackPresentationSpec();
    customSpec.presetId = rt::CustomPresetId{0xb};

    {
      auto runtimeOwnerPtr = rt::test::makeStateOnlyRuntime(tempDir);
      auto& runtime = runtimeOwnerPtr->runtime();
      REQUIRE(runtime.workspace().addCustomPreset(rt::CustomTrackPresentationPreset{
        .label = "Restored Custom",
        .optBasePresetId = rt::BuiltinPreset::Songs,
        .spec = customSpec,
      }));
      runtime.workspace().saveSession(runtime.workspaceConfigStore());
    }

    auto runtimeOwnerPtr = rt::test::makeStateOnlyRuntime(tempDir);
    auto& runtime = runtimeOwnerPtr->runtime();
    REQUIRE(runtime.workspace().restoreSession(runtime.workspaceConfigStore()));
    rt::test::settleRuntimeCallbacks(runtime);

    auto presentationCatalog =
      uimodel::TrackPresentationCatalog{runtime.workspace(), ao::test::englishMessageCatalog()};
    auto listPresentations = uimodel::ListPresentations{presentationCatalog, runtime.library().changes()};
    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto controller = LibraryController{runtime.library(),
                                        runtime.views(),
                                        runtime.workspace(),
                                        ao::test::englishMessageCatalog(),
                                        listPresentations,
                                        columnLayouts};

    auto const entryIt = std::ranges::find(
      controller.presentationEntries(), std::string_view{"000000000000000b"}, &TrackPresentationNavEntry::id);
    REQUIRE(entryIt != controller.presentationEntries().end());
    CHECK(entryIt->label == "Restored Custom");
  }

  TEST_CASE("LibraryController - presentation list follows custom preset changes")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("First");

    auto controller = fixture.makeController();
    auto const customId = rt::CustomPresetId{0xc};
    auto customSpec = rt::defaultTrackPresentationSpec();
    customSpec.presetId = customId;
    auto const beforePreset = controller.activePresentation().presetId;
    auto const initialCount = controller.presentationEntries().size();
    REQUIRE(fixture.runtime().workspace().addCustomPreset(rt::CustomTrackPresentationPreset{
      .label = "Custom Songs",
      .optBasePresetId = rt::BuiltinPreset::Songs,
      .spec = customSpec,
    }));
    rt::test::settleRuntimeCallbacks(fixture.runtime());

    REQUIRE(controller.presentationEntries().size() == initialCount + 1);
    CHECK(controller.presentationEntries().back().id == "000000000000000c");
    REQUIRE(controller.selectedPresentation() >= 0);
    CHECK(controller.presentationEntries()[static_cast<std::size_t>(controller.selectedPresentation())].id ==
          rt::trackPresetKey(beforePreset));

    CHECK_FALSE(controller.trySetSelectedPresentation(-1));
    CHECK_FALSE(
      controller.trySetSelectedPresentation(static_cast<std::int32_t>(controller.presentationEntries().size())));
    REQUIRE(
      controller.trySetSelectedPresentation(static_cast<std::int32_t>(controller.presentationEntries().size()) - 1));

    CHECK(controller.selectSelectedPresentation() == "View: 000000000000000c");
    CHECK(fixture.runtime().views().trackListState(controller.activeViewId()).presentation.isPreset(customId));
    CHECK(fixture.listPresentations.presentationIdForList(rt::kAllTracksListId) == rt::TrackPresetId{customId});
    CHECK(controller.selectedPresentation() == static_cast<std::int32_t>(controller.presentationEntries().size()) - 1);
  }

  TEST_CASE("LibraryController - setPresentation preserves selected track identity")
  {
    auto fixture = LibraryControllerFixture{};
    auto const olderId = fixture.addTrack(library::test::TrackSpec{
      .title = "Older", .artist = "Same Artist", .album = "Z Album", .uri = "older.flac", .year = 2000});
    fixture.addTrack(library::test::TrackSpec{
      .title = "Newer", .artist = "Same Artist", .album = "A Album", .uri = "newer.flac", .year = 2025});

    auto controller = fixture.makeController();
    REQUIRE(controller.tracks().size() == 2);
    REQUIRE(controller.tracks()[1].id == olderId);

    controller.moveTrackSelection(1);
    REQUIRE(controller.selectedTrackView().track != nullptr);
    REQUIRE(controller.selectedTrackView().track->id == olderId);

    CHECK(controller.setPresentation(rt::BuiltinPreset::Artists) == "View: artists");
    REQUIRE(controller.selectedTrackView().track != nullptr);
    CHECK(controller.selectedTrackView().track->id == olderId);
    CHECK(controller.selectedTrack() == 0);
  }

  TEST_CASE("LibraryController - album presentation exposes projection sections")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack(library::test::TrackSpec{
      .title = "A One", .artist = "Artist", .album = "Album A", .albumArtist = "Artist", .year = 2020});
    fixture.addTrack(library::test::TrackSpec{
      .title = "A Two", .artist = "Artist", .album = "Album A", .albumArtist = "Artist", .year = 2020});
    fixture.addTrack(library::test::TrackSpec{
      .title = "B One", .artist = "Artist", .album = "Album B", .albumArtist = "Artist", .year = 2021});

    auto controller = fixture.makeController();

    CHECK(controller.setPresentation(rt::BuiltinPreset::Albums) == "View: albums");
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

  TEST_CASE("LibraryController - section jumps keep track selection flat")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack(
      library::test::TrackSpec{.title = "A One", .artist = "Artist", .album = "Album A", .albumArtist = "Artist"});
    fixture.addTrack(
      library::test::TrackSpec{.title = "A Two", .artist = "Artist", .album = "Album A", .albumArtist = "Artist"});
    fixture.addTrack(
      library::test::TrackSpec{.title = "B One", .artist = "Artist", .album = "Album B", .albumArtist = "Artist"});

    auto controller = fixture.makeController();
    REQUIRE(controller.setPresentation(rt::BuiltinPreset::Albums) == "View: albums");
    REQUIRE(controller.sections().size() == 2);

    controller.setSelectedTrackIndex(1);
    CHECK(controller.jumpToAdjacentSection(1) == "Section: Album B");
    CHECK(controller.selectedTrack() == 2);

    CHECK(controller.jumpToAdjacentSection(-1) == "Section: Album A");
    CHECK(controller.selectedTrack() == 0);
  }

  TEST_CASE("LibraryController - section-boundary jumps preserve focus and effective selection")
  {
    auto const delta = GENERATE(-1, 1);
    CAPTURE(delta);
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack(library::test::TrackSpec{.title = "A 1", .album = "Album A", .albumArtist = "Artist"});
    auto const firstBoundaryId =
      fixture.addTrack(library::test::TrackSpec{.title = "A 2", .album = "Album A", .albumArtist = "Artist"});
    fixture.addTrack(library::test::TrackSpec{.title = "B 1", .album = "Album B", .albumArtist = "Artist"});
    fixture.addTrack(library::test::TrackSpec{.title = "B 2", .album = "Album B", .albumArtist = "Artist"});
    auto const lastBoundaryId =
      fixture.addTrack(library::test::TrackSpec{.title = "B 3", .album = "Album B", .albumArtist = "Artist"});
    auto controller = fixture.makeController();
    REQUIRE(controller.setPresentation(rt::BuiltinPreset::Albums) == "View: albums");
    REQUIRE(controller.sections().size() == 2);
    REQUIRE(controller.sections()[0].rowBegin == 0);
    REQUIRE(controller.sections()[0].rowCount == 2);
    REQUIRE(controller.sections()[1].rowBegin == 2);
    REQUIRE(controller.sections()[1].rowCount == 3);
    auto const selectedIndex = delta < 0 ? 1 : 4;
    auto const expectedId = delta < 0 ? firstBoundaryId : lastBoundaryId;
    controller.setSelectedTrackIndex(selectedIndex);
    REQUIRE(controller.selectedTrackView().track != nullptr);
    REQUIRE(controller.selectedTrackView().track->id == expectedId);

    SECTION("unmarked focus is the effective selection")
    {
      REQUIRE(controller.markedIds().empty());
      REQUIRE_FALSE(controller.isVisualSelectionActive());
    }

    SECTION("independent marks survive the boundary")
    {
      controller.setSelectedTrackIndex(0);
      controller.toggleFocusedMark();
      controller.setSelectedTrackIndex(selectedIndex);
      REQUIRE(controller.markedIds().size() == 1);
    }

    SECTION("an active visual range survives the boundary")
    {
      controller.setSelectedTrackIndex(selectedIndex - 1);
      controller.toggleVisualSelection();
      controller.setSelectedTrackIndex(selectedIndex);
      REQUIRE(controller.isVisualSelectionActive());
      REQUIRE(controller.selectedTrackIds().size() == 2);
    }

    auto const marksBefore = controller.markedIds();
    auto const selectionBefore = controller.selectedTrackIds();
    auto const visualBefore = controller.isVisualSelectionActive();
    auto const publishedBefore = fixture.runtime().views().trackListState(controller.activeViewId()).selection;
    REQUIRE(publishedBefore == selectionBefore);

    for (std::int32_t attempt = 0; attempt < 2; ++attempt)
    {
      CAPTURE(attempt);
      CHECK(controller.jumpToAdjacentSection(delta) == (delta < 0 ? "Section: Album A" : "Section: Album B"));
      CHECK(controller.selectedTrack() == selectedIndex);
      REQUIRE(controller.selectedTrackView().track != nullptr);
      CHECK(controller.selectedTrackView().track->id == expectedId);
      CHECK(controller.markedIds() == marksBefore);
      CHECK(controller.selectedTrackIds() == selectionBefore);
      CHECK(controller.isVisualSelectionActive() == visualBefore);
      CHECK(fixture.runtime().views().trackListState(controller.activeViewId()).selection == publishedBefore);
    }
  }

  TEST_CASE("LibraryController - single-section jumps preserve an interior row")
  {
    auto const delta = GENERATE(-1, 1);
    CAPTURE(delta);
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack(library::test::TrackSpec{.title = "One", .album = "Only Album", .albumArtist = "Artist"});
    auto const secondId =
      fixture.addTrack(library::test::TrackSpec{.title = "Two", .album = "Only Album", .albumArtist = "Artist"});
    auto controller = fixture.makeController();
    REQUIRE(controller.setPresentation(rt::BuiltinPreset::Albums) == "View: albums");
    REQUIRE(controller.sections().size() == 1);
    REQUIRE(controller.sections()[0].rowCount == 2);
    controller.setSelectedTrackIndex(1);
    REQUIRE(controller.selectedTrackView().track != nullptr);
    REQUIRE(controller.selectedTrackView().track->id == secondId);
    REQUIRE(controller.jumpToAdjacentSection(delta) == "Section: Only Album");
    CHECK(controller.selectedTrack() == 1);
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{secondId});
    CHECK(fixture.runtime().views().trackListState(controller.activeViewId()).selection ==
          std::vector<TrackId>{secondId});
  }

  TEST_CASE("LibraryController - section selection reports empty and invalid states")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("First");

    auto controller = fixture.makeController();
    REQUIRE(controller.setPresentation(rt::BuiltinPreset::Songs) == "View: songs");
    REQUIRE(controller.sections().empty());

    CHECK(controller.jumpToAdjacentSection(1) == "No sections in this view");
    CHECK(controller.selectSection(0) == "No section selected");
    CHECK(controller.selectSection(-1) == "No section selected");
    CHECK(controller.selectedTrack() == 0);
  }

  TEST_CASE("LibraryController - marks are independent of focus and publish effective ids")
  {
    auto fixture = LibraryControllerFixture{};
    auto const firstId = fixture.addTrack("First");
    auto const secondId = fixture.addTrack("Second");
    auto const thirdId = fixture.addTrack("Third");
    auto controller = fixture.makeController();
    REQUIRE(controller.tracks().size() == 3);

    controller.toggleFocusedMark();
    CHECK(controller.markedIds().contains(firstId));
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{firstId});

    controller.moveTrackSelection(1);
    CHECK(controller.selectedTrack() == 1);
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{firstId});

    controller.toggleFocusedMark();
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{firstId, secondId});

    controller.moveTrackSelection(1);
    CHECK(controller.selectedTrackView().track->id == thirdId);
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{firstId, secondId});

    auto const& published = fixture.runtime().views().trackListState(controller.activeViewId()).selection;
    CHECK(published == std::vector<TrackId>{firstId, secondId});
  }

  TEST_CASE("LibraryController - a visual selection grows with the focus and confirms into the marked set")
  {
    auto fixture = LibraryControllerFixture{};
    auto const firstId = fixture.addTrack("First");
    auto const secondId = fixture.addTrack("Second");
    auto const thirdId = fixture.addTrack("Third");
    auto controller = fixture.makeController();

    controller.toggleVisualSelection();
    CHECK(controller.isVisualSelectionActive());
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{firstId});

    controller.moveTrackSelection(2);
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{firstId, secondId, thirdId});

    // Retreating re-derives the range instead of accumulating what it passed over.
    controller.moveTrackSelection(-1);
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{firstId, secondId});

    controller.toggleVisualSelection();
    CHECK_FALSE(controller.isVisualSelectionActive());
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{firstId, secondId});

    controller.moveTrackSelection(1);
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{firstId, secondId});
  }

  TEST_CASE("LibraryController - cancelling a visual selection restores the marks it started from")
  {
    auto fixture = LibraryControllerFixture{};
    auto const firstId = fixture.addTrack("First");
    auto const secondId = fixture.addTrack("Second");
    auto const thirdId = fixture.addTrack("Third");
    auto controller = fixture.makeController();

    controller.toggleFocusedMark();
    controller.setSelectedTrackIndex(1);
    controller.toggleVisualSelection();
    controller.moveTrackSelection(1);
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{firstId, secondId, thirdId});

    controller.cancelVisualSelection();
    CHECK_FALSE(controller.isVisualSelectionActive());
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{firstId});

    auto const& published = fixture.runtime().views().trackListState(controller.activeViewId()).selection;
    CHECK(published == std::vector<TrackId>{firstId});
  }

  TEST_CASE("LibraryController - toggling a mark commits the running visual range first")
  {
    auto fixture = LibraryControllerFixture{};
    auto const firstId = fixture.addTrack("First");
    auto const secondId = fixture.addTrack("Second");
    auto const thirdId = fixture.addTrack("Third");
    auto controller = fixture.makeController();

    controller.toggleVisualSelection();
    controller.moveTrackSelection(2);
    controller.toggleFocusedMark();

    CHECK_FALSE(controller.isVisualSelectionActive());
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{firstId, secondId});
    CHECK_FALSE(controller.markedIds().contains(thirdId));

    // With the range committed, a later move no longer re-derives it.
    controller.moveTrackSelection(-2);
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{firstId, secondId});
  }

  TEST_CASE("LibraryController - a visual selection crossing its anchor derives the range on the other side")
  {
    auto fixture = LibraryControllerFixture{};
    auto const firstId = fixture.addTrack("First");
    auto const secondId = fixture.addTrack("Second");
    auto const thirdId = fixture.addTrack("Third");
    auto controller = fixture.makeController();

    controller.moveTrackSelection(1);
    controller.toggleVisualSelection();
    controller.moveTrackSelection(1);
    REQUIRE(controller.selectedTrackIds() == std::vector<TrackId>{secondId, thirdId});

    // Crossing back over the anchor drops the rows on the far side instead of
    // keeping everything the focus ever passed.
    controller.moveTrackSelection(-2);
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{firstId, secondId});
    CHECK_FALSE(controller.markedIds().contains(thirdId));
  }

  TEST_CASE("LibraryController - marking all or clearing ends a running visual selection")
  {
    auto fixture = LibraryControllerFixture{};
    auto const firstId = fixture.addTrack("First");
    auto const secondId = fixture.addTrack("Second");
    fixture.addTrack("Third");

    SECTION("marking all takes over the whole view")
    {
      auto controller = fixture.makeController();
      controller.toggleVisualSelection();
      controller.markAllTracks();

      CHECK_FALSE(controller.isVisualSelectionActive());
      CHECK(controller.markedIds().size() == controller.tracks().size());

      controller.moveTrackSelection(1);
      CHECK(controller.markedIds().size() == controller.tracks().size());
    }

    SECTION("clearing drops the range instead of leaving it running")
    {
      auto controller = fixture.makeController();
      controller.toggleVisualSelection();
      controller.moveTrackSelection(1);
      REQUIRE(controller.selectedTrackIds() == std::vector<TrackId>{firstId, secondId});

      controller.clearMarks();
      CHECK_FALSE(controller.isVisualSelectionActive());
      CHECK(controller.markedIds().empty());

      controller.moveTrackSelection(1);
      CHECK(controller.markedIds().empty());
    }
  }

  TEST_CASE("LibraryController - a running visual selection derives its range again in a new row order")
  {
    auto fixture = LibraryControllerFixture{};
    auto const alphaId = fixture.addTrack(library::test::TrackSpec{
      .title = "Alpha", .artist = "Same Artist", .album = "Alpha Album", .uri = "alpha.flac", .year = 2000});
    auto const bravoId = fixture.addTrack(library::test::TrackSpec{
      .title = "Bravo", .artist = "Same Artist", .album = "Bravo Album", .uri = "bravo.flac", .year = 2020});
    auto const charlieId = fixture.addTrack(library::test::TrackSpec{
      .title = "Charlie", .artist = "Same Artist", .album = "Charlie Album", .uri = "charlie.flac", .year = 2010});
    auto controller = fixture.makeController();
    REQUIRE(controller.tracks().size() == 3);
    REQUIRE(controller.tracks()[0].id == alphaId);
    REQUIRE(controller.tracks()[1].id == bravoId);
    REQUIRE(controller.tracks()[2].id == charlieId);

    controller.toggleVisualSelection();
    controller.moveTrackSelection(1);
    REQUIRE(controller.selectedTrackIds() == std::vector<TrackId>{alphaId, bravoId});

    CHECK(controller.setPresentation(rt::BuiltinPreset::Artists) == "View: artists");
    REQUIRE(controller.tracks()[0].id == alphaId);
    REQUIRE(controller.tracks()[1].id == charlieId);
    REQUIRE(controller.tracks()[2].id == bravoId);

    // The range is a span of rows, so the row that moved between the anchor and
    // the focus joins it instead of leaving a gap until the next motion.
    CHECK(controller.isVisualSelectionActive());
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{alphaId, charlieId, bravoId});

    controller.moveTrackSelection(-1);
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{alphaId, charlieId});
  }

  TEST_CASE("LibraryController - a no-op filter keeps the selection running and follows the focused track")
  {
    auto fixture = LibraryControllerFixture{};
    auto const alphaId = fixture.addTrack(library::test::TrackSpec{
      .title = "Alpha", .artist = "Same Artist", .album = "Alpha Album", .uri = "alpha.flac", .year = 2000});
    auto const bravoId = fixture.addTrack(library::test::TrackSpec{
      .title = "Bravo", .artist = "Same Artist", .album = "Bravo Album", .uri = "bravo.flac", .year = 2020});
    auto const charlieId = fixture.addTrack(library::test::TrackSpec{
      .title = "Charlie", .artist = "Same Artist", .album = "Charlie Album", .uri = "charlie.flac", .year = 2010});
    auto controller = fixture.makeController();
    REQUIRE(controller.tracks()[1].id == bravoId);

    controller.toggleVisualSelection();
    controller.moveTrackSelection(1);
    REQUIRE(controller.selectedTrackIds() == std::vector<TrackId>{alphaId, bravoId});

    // Another shell can reorder the rows under the same filter expression, so
    // the kept focus has to follow its track rather than its row index.
    REQUIRE(fixture.runtime().workspace().setActivePresentation(rt::BuiltinPreset::Artists));
    REQUIRE(controller.applyFilter());

    REQUIRE(controller.tracks()[2].id == bravoId);
    REQUIRE(controller.selectedTrackView().track != nullptr);
    CHECK(controller.selectedTrackView().track->id == bravoId);
    CHECK(controller.isVisualSelectionActive());
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{alphaId, charlieId, bravoId});
  }

  TEST_CASE("LibraryController - a visual selection survives view work that keeps the view and ends work that does not")
  {
    auto fixture = LibraryControllerFixture{};
    auto const alphaId = fixture.addTrack(library::test::TrackSpec{
      .title = "Alpha", .artist = "Same Artist", .album = "Alpha Album", .uri = "alpha.flac", .year = 2000});
    auto const bravoId = fixture.addTrack(library::test::TrackSpec{
      .title = "Bravo", .artist = "Same Artist", .album = "Bravo Album", .uri = "bravo.flac", .year = 2020});
    auto const charlieId = fixture.addTrack(library::test::TrackSpec{
      .title = "Charlie", .artist = "Same Artist", .album = "Charlie Album", .uri = "charlie.flac", .year = 2010});

    SECTION("a rejected filter leaves the running selection exactly as it was")
    {
      auto controller = fixture.makeController();
      auto const activeViewId = controller.activeViewId();
      controller.toggleVisualSelection();
      controller.moveTrackSelection(1);
      REQUIRE(controller.selectedTrackIds() == std::vector<TrackId>{alphaId, bravoId});

      controller.setFilterDraft("Alpha");
      REQUIRE(fixture.runtime().workspace().closeView(activeViewId));
      CHECK_FALSE(controller.applyFilter());

      CHECK(controller.isVisualSelectionActive());
      CHECK(controller.selectedTrackIds() == std::vector<TrackId>{alphaId, bravoId});
    }

    SECTION("a new filter expression ends the selection with the marks it cleared")
    {
      auto controller = fixture.makeController();
      controller.toggleVisualSelection();
      controller.moveTrackSelection(1);
      REQUIRE(controller.selectedTrackIds() == std::vector<TrackId>{alphaId, bravoId});

      controller.setFilterDraft("Alpha");
      REQUIRE(controller.applyFilter());

      CHECK_FALSE(controller.isVisualSelectionActive());
      CHECK(controller.markedIds().empty());
    }

    SECTION("reattaching the open view keeps the selection running")
    {
      auto controller = fixture.makeController();
      controller.toggleVisualSelection();
      controller.moveTrackSelection(1);
      REQUIRE(controller.selectedTrackIds() == std::vector<TrackId>{alphaId, bravoId});

      REQUIRE(controller.openList(controller.navigation().cursor()));

      CHECK(controller.isVisualSelectionActive());
      CHECK(controller.selectedTrackIds() == std::vector<TrackId>{alphaId, bravoId});

      controller.moveTrackSelection(1);
      CHECK(controller.selectedTrackIds() == std::vector<TrackId>{alphaId, bravoId, charlieId});
    }

    SECTION("a reorder keeps the marks the selection started from")
    {
      auto controller = fixture.makeController();
      controller.moveTrackSelection(1);
      controller.toggleFocusedMark();
      controller.moveTrackSelection(-1);
      controller.toggleVisualSelection();
      REQUIRE(controller.selectedTrackIds() == std::vector<TrackId>{alphaId, bravoId});

      CHECK(controller.setPresentation(rt::BuiltinPreset::Artists) == "View: artists");
      REQUIRE(controller.tracks()[1].id == charlieId);

      // The starting set is carried through the new row order untouched, and the
      // range is only the anchor row until the focus moves again.
      CHECK(controller.isVisualSelectionActive());
      CHECK(controller.selectedTrackIds() == std::vector<TrackId>{alphaId, bravoId});

      controller.moveTrackSelection(1);
      CHECK(controller.selectedTrackIds() == std::vector<TrackId>{alphaId, charlieId, bravoId});
    }
  }

  TEST_CASE("LibraryController - a reload derives the visual range from the restored focus")
  {
    auto fixture = LibraryControllerFixture{};
    auto const alphaId = fixture.addTrack("Alpha");
    auto const bravoId = fixture.addTrack("Bravo");
    auto const charlieId = fixture.addTrack("Charlie");
    auto const deltaId = fixture.addTrack("Delta");
    auto controller = fixture.makeController();
    REQUIRE(controller.tracks().size() == 4);

    controller.toggleVisualSelection();
    controller.moveTrackSelection(2);
    REQUIRE(controller.selectedTrackIds() == std::vector<TrackId>{alphaId, bravoId, charlieId});

    REQUIRE(rt::test::runRuntimeTask(
      fixture.runtime(), fixture.runtimeOwnerPtr->libraryCommands().deleteTrackAsync(bravoId)));
    rt::test::settleRuntimeCallbacks(fixture.runtime());

    // The library observer already reloaded the view, so this is the state a
    // single reload leaves behind.
    REQUIRE(controller.tracks().size() == 3);

    // Dropping a row above the focus moves that row up, so the range has to
    // follow the restored focus instead of the index it used to hold.
    REQUIRE(controller.selectedTrackView().track != nullptr);
    CHECK(controller.selectedTrackView().track->id == charlieId);
    CHECK(controller.isVisualSelectionActive());
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{alphaId, charlieId});
    CHECK_FALSE(controller.markedIds().contains(deltaId));
  }

  TEST_CASE("LibraryController - a visual selection whose anchor vanishes ends and keeps the rows it reached")
  {
    auto fixture = LibraryControllerFixture{};
    auto const firstId = fixture.addTrack("First");
    auto const secondId = fixture.addTrack("Second");
    fixture.addTrack("Third");
    auto controller = fixture.makeController();

    controller.toggleVisualSelection();
    controller.moveTrackSelection(1);
    REQUIRE(controller.selectedTrackIds() == std::vector<TrackId>{firstId, secondId});

    REQUIRE(rt::test::runRuntimeTask(
      fixture.runtime(), fixture.runtimeOwnerPtr->libraryCommands().deleteTrackAsync(firstId)));
    rt::test::settleRuntimeCallbacks(fixture.runtime());
    CHECK(controller.reloadActiveList() == "Reloaded 2 tracks");

    // Without its anchor the range cannot be extended or cancelled coherently,
    // so it ends where it stood rather than restoring the starting set.
    CHECK_FALSE(controller.isVisualSelectionActive());
    CHECK(controller.markedIds() == std::unordered_set<TrackId>{secondId});
  }

  TEST_CASE("LibraryController - select all marks every materialized row and clear restores focus selection")
  {
    auto fixture = LibraryControllerFixture{};
    auto const firstId = fixture.addTrack("First");
    fixture.addTrack("Second");
    fixture.addTrack("Third");
    auto controller = fixture.makeController();

    controller.markAllTracks();
    CHECK(controller.markedIds().size() == 3);
    CHECK(controller.selectedTrackIds().size() == 3);

    controller.clearMarks();
    CHECK(controller.markedIds().empty());
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{firstId});
  }

  TEST_CASE("LibraryController - successful filter change clears marks; a no-op filter keeps them")
  {
    auto fixture = LibraryControllerFixture{};
    auto const firstId = fixture.addTrack("Alpha");
    fixture.addTrack("Beta");
    auto controller = fixture.makeController();

    controller.toggleFocusedMark();
    controller.setFilterDraft("Alpha");
    CHECK(requireAppliedFilter(controller) == "Quick filter matched 1 track");
    CHECK(controller.markedIds().empty());
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{firstId});

    controller.toggleFocusedMark();
    controller.setFilterDraft("Alpha");
    CHECK(requireAppliedFilter(controller) == "Quick filter matched 1 track");
    CHECK(controller.markedIds().contains(firstId));
  }

  TEST_CASE("LibraryController - presentation change keeps marks in the new row order")
  {
    auto fixture = LibraryControllerFixture{};
    auto const olderId = fixture.addTrack(library::test::TrackSpec{
      .title = "Older", .artist = "Same Artist", .album = "Z Album", .uri = "older.flac", .year = 2000});
    auto const newerId = fixture.addTrack(library::test::TrackSpec{
      .title = "Newer", .artist = "Same Artist", .album = "A Album", .uri = "newer.flac", .year = 2025});
    auto controller = fixture.makeController();
    REQUIRE(controller.tracks().size() == 2);
    REQUIRE(controller.tracks()[0].id == newerId);
    REQUIRE(controller.tracks()[1].id == olderId);

    controller.toggleFocusedMark();
    controller.moveTrackSelection(1);
    controller.toggleFocusedMark();
    REQUIRE(controller.selectedTrack() == 1);
    REQUIRE(controller.selectedTrackIds() == std::vector<TrackId>{newerId, olderId});

    CHECK(controller.setPresentation(rt::BuiltinPreset::Artists) == "View: artists");
    REQUIRE(controller.selectedTrackView().track != nullptr);
    CHECK(controller.selectedTrackView().track->id == olderId);
    CHECK(controller.selectedTrackIds() == std::vector<TrackId>{olderId, newerId});
    CHECK(fixture.runtime().views().trackListState(controller.activeViewId()).selection ==
          std::vector<TrackId>{olderId, newerId});
  }

  TEST_CASE("LibraryController - same-view reload reconciles vanished marks")
  {
    auto fixture = LibraryControllerFixture{};
    auto const firstId = fixture.addTrack("First");
    auto const secondId = fixture.addTrack("Second");
    auto controller = fixture.makeController();

    controller.toggleFocusedMark();
    controller.moveTrackSelection(1);
    controller.toggleFocusedMark();
    REQUIRE(controller.selectedTrackIds() == std::vector<TrackId>{firstId, secondId});

    SECTION("deleting a marked track drops that mark")
    {
      REQUIRE(rt::test::runRuntimeTask(
        fixture.runtime(), fixture.runtimeOwnerPtr->libraryCommands().deleteTrackAsync(secondId)));
      rt::test::settleRuntimeCallbacks(fixture.runtime());
      CHECK(controller.reloadActiveList() == "Reloaded 1 track");
      CHECK(controller.markedIds() == std::unordered_set<TrackId>{firstId});
      CHECK(controller.selectedTrackIds() == std::vector<TrackId>{firstId});
      CHECK(fixture.runtime().views().trackListState(controller.activeViewId()).selection ==
            std::vector<TrackId>{firstId});
    }

    SECTION("deleting another marked track keeps the remaining mark")
    {
      REQUIRE(rt::test::runRuntimeTask(
        fixture.runtime(), fixture.runtimeOwnerPtr->libraryCommands().deleteTrackAsync(firstId)));
      rt::test::settleRuntimeCallbacks(fixture.runtime());
      CHECK(controller.reloadActiveList() == "Reloaded 1 track");
      CHECK(controller.markedIds() == std::unordered_set<TrackId>{secondId});
      CHECK(controller.selectedTrackIds() == std::vector<TrackId>{secondId});
      CHECK(fixture.runtime().views().trackListState(controller.activeViewId()).selection ==
            std::vector<TrackId>{secondId});
    }
  }

  TEST_CASE("LibraryController - list navigation clears marks; same-view reload keeps live ids")
  {
    auto fixture = LibraryControllerFixture{};
    auto const firstId = fixture.addTrack("Keep");
    fixture.addTrack("Other");
    auto const listId = fixture.addList("Side");
    auto controller = fixture.makeController();

    controller.toggleFocusedMark();
    CHECK(controller.reloadActiveList().contains("2"));
    CHECK(controller.markedIds().contains(firstId));

    controller.navigation().reveal(listId);
    REQUIRE(controller.openList(controller.navigation().cursor()));
    CHECK(controller.markedIds().empty());
  }

  TEST_CASE("LibraryController - reopening the current list keeps marks, focus, and published selection")
  {
    auto fixture = LibraryControllerFixture{};
    auto const firstId = fixture.addTrack("First");
    auto const secondId = fixture.addTrack("Second");
    auto controller = fixture.makeController();

    controller.setFilterDraft("$title?");
    REQUIRE(controller.applyFilter());
    controller.toggleFocusedMark();
    controller.moveTrackSelection(1);
    controller.toggleFocusedMark();
    auto const viewId = controller.activeViewId();
    auto const marked = controller.selectedTrackIds();
    REQUIRE(viewId != rt::kInvalidViewId);
    REQUIRE(marked == std::vector<TrackId>{firstId, secondId});

    auto const workspaceBefore = fixture.runtime().workspace().snapshot();
    auto const openedRes = controller.openList(controller.currentListId());
    REQUIRE(openedRes);
    CHECK_FALSE(*openedRes);
    CHECK(fixture.runtime().workspace().snapshot() == workspaceBefore);
    CHECK(controller.filterDraft() == "$title?");

    CHECK(controller.activeViewId() == viewId);
    CHECK(controller.markedIds().contains(firstId));
    CHECK(controller.markedIds().contains(secondId));
    CHECK(controller.selectedTrackIds() == marked);
    CHECK(fixture.runtime().views().trackListState(viewId).selection == marked);
    CHECK(controller.selectedTrack() == 1);
    REQUIRE(controller.selectedTrackView().track != nullptr);
    CHECK(controller.selectedTrackView().track->id == secondId);
  }

  TEST_CASE("LibraryController - empty library guidance uses the available scan command")
  {
    auto fixture = LibraryControllerFixture{};
    auto controller = fixture.makeController();
    REQUIRE(controller.tracks().empty());
    CHECK(controller.emptyStateText() == "No indexed tracks. Run :scan to scan this library.");
    controller.setFilterDraft("not applied yet");
    CHECK(controller.emptyStateText().contains(":scan"));
  }

  TEST_CASE("LibraryController - empty-state guidance recovers from unmatched and invalid filters")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("Alpha");
    auto controller = fixture.makeController();
    controller.setFilterDraft("missing");
    REQUIRE(controller.applyFilter());
    REQUIRE(controller.tracks().empty());
    CHECK(controller.emptyStateText() == "No tracks match this filter. Change or clear the filter.");
    controller.setFilterDraft("$artist =");
    REQUIRE(controller.applyFilter());
    REQUIRE_FALSE(controller.filterError().empty());
    CHECK(controller.emptyStateText() == "The filter could not be applied. Edit or clear it.");
    controller.clearFilterDraft();
    REQUIRE(controller.applyFilter());
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks().front().row.title == "Alpha");
  }

  TEST_CASE("LibraryController - empty-state guidance identifies an empty saved List")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("Alpha");
    auto const emptyList = ao::test::requireValue(
      rt::test::runRuntimeTask(fixture.runtime(),
                               fixture.runtimeOwnerPtr->libraryCommands().createListAsync(
                                 rt::ListDraft{.name = "Empty", .expression = "$title ~ \"Missing\""})));
    auto listController = fixture.makeController();

    REQUIRE(listController.openList(emptyList));
    REQUIRE(listController.currentListId() == emptyList);
    REQUIRE(listController.tracks().empty());
    CHECK(listController.emptyStateText() == "This List has no tracks. Choose another List to browse.");
  }

  TEST_CASE("LibraryController - reveal finishes a visual range without extending its marks")
  {
    auto fixture = LibraryControllerFixture{};
    auto const first = fixture.addTrack("First");
    auto const second = fixture.addTrack("Second");
    auto controller = fixture.makeController();
    controller.toggleVisualSelection();
    REQUIRE(controller.markedIds() == std::unordered_set<TrackId>{first});
    CHECK(ao::test::requireValue(controller.revealTrack(second)) == "Revealed Second");
    CHECK_FALSE(controller.isVisualSelectionActive());
    CHECK(controller.markedIds() == std::unordered_set<TrackId>{first});
    CHECK(controller.tracks()[controller.selectedTrack()].id == second);
  }

  TEST_CASE("LibraryController - reveal prefers a source view and tolerates a removed source")
  {
    auto fixture = LibraryControllerFixture{};
    auto const target = fixture.addTrack("Target");
    fixture.addTrack("Other");
    auto const sourceList = fixture.addList("Source");
    auto const sourceView = ao::test::requireValue(fixture.runtime().workspace().navigate({.target = sourceList}));
    auto controller = fixture.makeController();
    controller.setFilterDraft("Other");
    REQUIRE(controller.applyFilter());
    auto const filteredView = controller.activeViewId();
    auto const plainSource = ao::test::requireValue(fixture.runtime().workspace().navigate({.target = sourceList}));
    REQUIRE(fixture.runtime().workspace().focusView(filteredView));

    SECTION("An existing source view is reused")
    {
      CHECK(ao::test::requireValue(controller.revealTrack(target, plainSource, sourceList)) == "Revealed Target");
      CHECK(controller.activeViewId() == plainSource);
      REQUIRE(controller.navigateHistory(false));
      CHECK(controller.activeViewId() == filteredView);
    }

    SECTION("A closed source view falls back without changing the filtered view")
    {
      REQUIRE(fixture.runtime().workspace().closeView(plainSource));
      CHECK(ao::test::requireValue(controller.revealTrack(target, plainSource, kInvalidListId)) == "Revealed Target");
      CHECK(controller.currentListId() == rt::kAllTracksListId);
      CHECK_FALSE(fixture.runtime().views().trackListState(sourceView).filterExpression.empty());
    }
  }

  TEST_CASE("LibraryController - reveal of a missing subject leaves the workspace intact")
  {
    auto fixture = LibraryControllerFixture{};
    auto const firstId = fixture.addTrack("First");
    auto controller = fixture.makeController();
    auto const before = fixture.runtime().workspace().snapshot();
    auto const selected = controller.selectedTrackIds();
    REQUIRE(selected == std::vector{firstId});

    controller.setFilterDraft("unfinished");
    controller.toggleVisualSelection();
    auto const rowsRevision = controller.trackRowsRevision();
    bool selectionChanged = false;
    auto sub = fixture.runtime().views().onSelectionChanged([&](rt::ViewService::SelectionChanged const&)
                                                            { selectionChanged = true; });

    SECTION("Direct track lookup")
    {
      auto const res = controller.revealTrack(TrackId{999999});
      REQUIRE_FALSE(res);
      CHECK(res.error().message == "Could not locate the current track in the library");
    }

    SECTION("Playback source lookup")
    {
      auto const res = controller.revealPlayingTrack(TrackId{999999}, rt::kAllTracksListId);
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotFound);
      CHECK(res.error().message == "Could not locate the current track in the library");
    }

    CHECK_FALSE(selectionChanged);
    CHECK(controller.filterDraft() == "unfinished");
    CHECK(controller.isVisualSelectionActive());
    CHECK(controller.trackRowsRevision() == rowsRevision);
    CHECK(fixture.runtime().workspace().snapshot() == before);
    CHECK(controller.activeViewId() == before.activeViewId);
    CHECK(controller.selectedTrackIds() == selected);
    REQUIRE(controller.tracks().size() == 1);
    CHECK(controller.tracks().front().id == firstId);
  }

  TEST_CASE("LibraryController - missing List fails before changing active view or selection")
  {
    auto fixture = LibraryControllerFixture{};
    fixture.addTrack("Retained");
    auto controller = fixture.makeController();
    controller.toggleVisualSelection();
    auto const before = fixture.runtime().workspace().snapshot();
    auto const selected = controller.selectedTrackIds();
    auto const res = controller.openList(ListId{987654});
    REQUIRE_FALSE(res);
    CHECK(fixture.runtime().workspace().snapshot() == before);
    CHECK(controller.activeViewId() == before.activeViewId);
    CHECK(controller.selectedTrackIds() == selected);
    CHECK(controller.isVisualSelectionActive());
  }
} // namespace ao::tui::test
