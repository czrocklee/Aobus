// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/async/LoopExecutor.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/library/track/TrackFilterView.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;

  namespace
  {
    struct TrackFilterFixture final
    {
      MusicLibraryFixture libraryFixture;
      InlineExecutor executor;
      LibraryChanges changes{executor, 0, "test-library"};
      TrackSourceCache trackSourceCache{libraryFixture.library(), changes};
      ViewService viewService{executor, libraryFixture.library(), trackSourceCache, changes};
      WorkspaceService workspaceService{executor, viewService, changes};
      ao::test::RenderLog<TrackFilterViewState> renderLog;
      TrackFilterViewModel viewModel;

      explicit TrackFilterFixture(i18n::MessageCatalog textCatalog = ao::test::messageCatalog("en"))
        : viewModel{viewService, workspaceService, textCatalog, [this](auto const& view) { renderLog.render(view); }}
      {
      }

      rt::ViewId focusAllTracksView()
      {
        return ao::test::requireValue(workspaceService.navigate({.target = rt::GlobalViewKind::AllTracks}));
      }
    };
  } // namespace

  TEST_CASE("TrackFilterViewModel - shared views reconcile commits while preserving an unsubmitted draft",
            "[uimodel][regression][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    auto const viewId = fixture.focusAllTracksView();
    auto otherLog = ao::test::RenderLog<TrackFilterViewState>{};
    auto other = TrackFilterViewModel{fixture.viewService,
                                      fixture.workspaceService,
                                      ao::test::messageCatalog("en"),
                                      [&otherLog](auto const& state) { otherLog.render(state); }};
    fixture.viewModel.updateFilter("first");
    CHECK(otherLog.last().resolvedExpression == fixture.renderLog.last().resolvedExpression);
    CHECK(otherLog.last().canCreateSmartList);

    other.editFilter("draft");
    fixture.viewModel.updateFilter("$artist =");
    CHECK(otherLog.last().entryText == "draft");
    CHECK_FALSE(otherLog.last().canCreateSmartList);
    CHECK(fixture.renderLog.last().hasError);

    other.updateFilter("draft");
    CHECK(fixture.renderLog.last().resolvedExpression == otherLog.last().resolvedExpression);
    CHECK_FALSE(fixture.renderLog.last().hasError);
    CHECK(fixture.renderLog.last().canCreateSmartList);
    CHECK(fixture.viewService.trackListState(viewId).filterExpression == otherLog.last().resolvedExpression);

    other.updateFilter("$artist =");
    CHECK(fixture.renderLog.last().hasError);
    CHECK_FALSE(fixture.renderLog.last().canCreateSmartList);
    other.updateFilter("");
    CHECK(fixture.renderLog.last().entryText.empty());
    CHECK_FALSE(fixture.renderLog.last().hasError);
    CHECK_FALSE(fixture.renderLog.last().canCreateSmartList);
  }

  TEST_CASE("TrackFilterViewModel - queued commit notices render only changed filter state",
            "[uimodel][regression][track-filter]")
  {
    auto library = MusicLibraryFixture{};
    auto executor = QueuedExecutor{};
    auto changes = LibraryChanges{executor, 0, "test-library"};
    auto sources = TrackSourceCache{library.library(), changes};
    auto views = ViewService{executor, library.library(), sources, changes};
    auto workspace = WorkspaceService{executor, views, changes};
    auto log = ao::test::RenderLog<TrackFilterViewState>{};
    auto model = TrackFilterViewModel{
      views, workspace, ao::test::englishMessageCatalog(), [&log](auto const& state) { log.render(state); }};
    auto const viewId = ao::test::requireValue(workspace.navigate({.target = GlobalViewKind::AllTracks}));
    executor.drain();
    log.clear();

    model.updateFilter("first");
    REQUIRE(log.states.size() == 1);
    executor.drain();
    CHECK(log.states.size() == 1);
    CHECK(log.last().entryText == "first");

    REQUIRE(views.setFilter(viewId, "$year >"));
    REQUIRE(views.setFilter(viewId, "true"));
    executor.drain();
    REQUIRE(log.states.size() == 2);
    CHECK(log.last().entryText == "true");
    CHECK_FALSE(log.last().hasError);

    model.updateFilter("$year >");
    REQUIRE(log.states.size() == 3);
    CHECK(log.last().hasError);
    executor.drain();
    CHECK(log.states.size() == 3);

    model.updateFilter("second");
    model.editFilter("second draft");
    REQUIRE(log.states.size() == 5);
    executor.drain();
    CHECK(log.states.size() == 5);
    CHECK(log.last().entryText == "second draft");
    CHECK_FALSE(log.last().canCreateSmartList);
  }

  TEST_CASE("TrackFilterViewModel - focus changes discard drafts and restore each view's committed filter",
            "[uimodel][regression][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    auto const first = fixture.focusAllTracksView();
    fixture.viewModel.updateFilter("$year = 2020");
    fixture.viewModel.editFilter("first draft");
    auto const second = ao::test::requireValue(fixture.workspaceService.navigate(
      {.target = FilteredListTarget{.listId = kAllTracksListId, .filterExpression = {}}}));
    REQUIRE(first != second);
    CHECK(fixture.renderLog.last().entryText.empty());
    CHECK_FALSE(fixture.renderLog.last().canCreateSmartList);
    REQUIRE(fixture.viewService.setFilter(second, "true"));
    fixture.viewModel.editFilter("second draft");

    REQUIRE(fixture.workspaceService.focusView(first));
    CHECK(fixture.renderLog.last().entryText == "$year = 2020");
    CHECK(fixture.renderLog.last().canCreateSmartList);
    REQUIRE(fixture.workspaceService.focusView(second));
    CHECK(fixture.renderLog.last().entryText == "true");
    CHECK(fixture.renderLog.last().resolvedExpression == "true");
    CHECK(fixture.renderLog.last().canCreateSmartList);
  }

  TEST_CASE("TrackFilterViewModel - initial render produces disabled state", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};

    REQUIRE(!fixture.renderLog.empty());
    CHECK(fixture.renderLog.last().enabled == false);
    CHECK(fixture.renderLog.last().entryText.empty());
  }

  TEST_CASE("TrackFilterViewModel - empty filter text keeps creation disabled", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};

    fixture.viewModel.updateFilter("");

    CHECK(fixture.renderLog.last().enabled == false);
    CHECK(fixture.renderLog.last().entryText.empty());
    CHECK(fixture.renderLog.last().resolvedExpression.empty());
    CHECK(fixture.renderLog.last().canCreateSmartList == false);
  }

  TEST_CASE("TrackFilterViewModel - expression syntax becomes the resolved expression", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();

    fixture.viewModel.updateFilter("$artist ~ 'Beatles'");

    CHECK(fixture.renderLog.last().enabled == true);
    CHECK(fixture.renderLog.last().entryText == "$artist ~ 'Beatles'");
    CHECK(fixture.renderLog.last().resolvedExpression == "$artist ~ 'Beatles'");
    CHECK(fixture.renderLog.last().hasError == false);
    CHECK(fixture.renderLog.last().canCreateSmartList == true);
  }

  TEST_CASE("TrackFilterViewModel - plain text resolves to quick search expression",
            "[uimodel][unit][track-filter][regression]")
  {
    auto fixture = TrackFilterFixture{};
    auto const aimerTrackId =
      fixture.libraryFixture.addTrack(library::test::TrackSpec{.title = "Brave Shine", .artist = "Aimer"});
    fixture.libraryFixture.addTrack(library::test::TrackSpec{.title = "Hysteria", .artist = "Muse"});
    fixture.trackSourceCache.reloadAllTracks();
    auto const viewId = fixture.focusAllTracksView();

    fixture.viewModel.updateFilter("aimer");

    CHECK(fixture.renderLog.last().entryText == "aimer");
    CHECK(fixture.renderLog.last().resolvedExpression.contains("$title ~ \"aimer\""));
    CHECK(fixture.renderLog.last().resolvedExpression.contains("$artist ~ \"aimer\""));
    CHECK(fixture.renderLog.last().canCreateSmartList == true);
    auto const projectionPtr = ao::test::requireValue(fixture.viewService.findTrackListProjection(viewId));
    REQUIRE(projectionPtr != nullptr);
    REQUIRE(projectionPtr->size() == 1);
    CHECK(projectionPtr->trackIdAt(0) == aimerTrackId);
    CHECK_FALSE(fixture.viewService.trackListState(viewId).optFilterError);

    fixture.viewModel.updateFilter("");

    auto const clearedProjectionPtr = ao::test::requireValue(fixture.viewService.findTrackListProjection(viewId));
    REQUIRE(clearedProjectionPtr != nullptr);
    REQUIRE(clearedProjectionPtr->size() == 2);
  }

  TEST_CASE("TrackFilterViewModel - multiple plain text terms resolve to conjunction", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();

    fixture.viewModel.updateFilter("Beatles help");

    CHECK(fixture.renderLog.last().entryText == "Beatles help");
    CHECK(fixture.renderLog.last().resolvedExpression.contains(") and ("));
    CHECK(fixture.renderLog.last().canCreateSmartList == true);
  }

  TEST_CASE("TrackFilterViewModel - Quick filter uses full Unicode case folding",
            "[uimodel][unit][track-filter][unicode]")
  {
    auto fixture = TrackFilterFixture{};
    auto const matchingTrackId =
      fixture.libraryFixture.addTrack(library::test::TrackSpec{.title = "Die Straße", .artist = "Die Ärzte"});
    fixture.libraryFixture.addTrack(library::test::TrackSpec{.title = "Autobahn", .artist = "Kraftwerk"});
    fixture.trackSourceCache.reloadAllTracks();
    auto const viewId = fixture.focusAllTracksView();

    fixture.viewModel.updateFilter("STRASSE");

    auto const projectionPtr = ao::test::requireValue(fixture.viewService.findTrackListProjection(viewId));
    REQUIRE(projectionPtr != nullptr);
    REQUIRE(projectionPtr->size() == 1);
    CHECK(projectionPtr->trackIdAt(0) == matchingTrackId);
    CHECK_FALSE(fixture.viewService.trackListState(viewId).optFilterError);
  }

  TEST_CASE("TrackFilterViewModel - focused track view enables filtering", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    REQUIRE(fixture.workspaceService.navigate({.target = rt::GlobalViewKind::AllTracks}));

    REQUIRE(!fixture.renderLog.empty());
    CHECK(fixture.renderLog.last().enabled == true);
  }

  TEST_CASE("TrackFilterViewModel - filter edits update the focused view", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();
    fixture.renderLog.clear();

    fixture.viewModel.updateFilter("$artist ~ 'Beatles'");

    REQUIRE(fixture.renderLog.states.size() == 1);
    CHECK(fixture.renderLog.last().entryText == "$artist ~ 'Beatles'");
    CHECK(fixture.renderLog.last().resolvedExpression == "$artist ~ 'Beatles'");
    CHECK(fixture.renderLog.last().hasError == false);
    CHECK(fixture.renderLog.last().canCreateSmartList == true);
  }

  TEST_CASE("TrackFilterViewModel - invalid expression exposes the runtime error", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();

    fixture.viewModel.updateFilter("$year >");

    CHECK(fixture.renderLog.last().entryText == "$year >");
    CHECK(fixture.renderLog.last().resolvedExpression == "$year >");
    CHECK(fixture.renderLog.last().hasError == true);
    CHECK(fixture.renderLog.last().tooltip.contains("Filter error"));
    CHECK(fixture.renderLog.last().canCreateSmartList == false);
  }

  TEST_CASE("TrackFilterViewModel - retains a temporary catalog for later renders",
            "[uimodel][regression][track-filter]")
  {
    auto fixture = TrackFilterFixture{ao::test::messageCatalog("de-DE")};
    fixture.focusAllTracksView();

    fixture.viewModel.updateFilter("$year >");

    CHECK(fixture.renderLog.last().hasError == true);
    CHECK(fixture.renderLog.last().tooltip.starts_with("Filterfehler:"));
  }

  TEST_CASE("TrackFilterViewModel - repaired stored source error refreshes the active view",
            "[uimodel][unit][track-filter]")
  {
    auto libraryFixture = MusicLibraryFixture{};
    auto listId = kInvalidListId;

    {
      auto transaction = library::test::writeTransaction(libraryFixture.library());
      auto builder = library::ListBuilder::makeEmpty().name("Invalid source").filter("(");
      listId = ao::test::requireValue(
        transaction.apply([&builder](library::LibraryWrite& write) { return write.lists().create(builder); }));
      REQUIRE(transaction.commit());
    }

    auto executor = async::LoopExecutor{};
    auto const revision = [&libraryFixture]
    {
      auto transaction = libraryFixture.library().readTransaction();
      return libraryFixture.library().libraryRevision(transaction);
    }();
    auto changes = LibraryChanges{executor, revision, "test-library"};
    auto commandsFixture = LibraryCommandsFixture{libraryFixture.library(), changes, executor};
    auto sources = TrackSourceCache{libraryFixture.library(), changes};
    auto views = ViewService{executor, libraryFixture.library(), sources, changes};
    auto workspace = WorkspaceService{executor, views, changes};
    auto renderLog = ao::test::RenderLog<TrackFilterViewState>{};
    auto viewModel = TrackFilterViewModel{
      views, workspace, ao::test::englishMessageCatalog(), [&renderLog](auto const& view) { renderLog.render(view); }};

    REQUIRE(
      workspace.navigate(NavigationRequest{.target = FilteredListTarget{.listId = listId, .filterExpression = {}}}));

    while (executor.tryRunReadyTurn())
    {
    }

    REQUIRE(renderLog.last().hasError);
    CHECK(renderLog.last().tooltip.contains("Filter error"));

    REQUIRE(commandsFixture.runTask(commandsFixture.commands().updateListAsync(ListDraft{
      .listId = listId,
      .name = "Repaired source",
      .expression = "true",
    })));

    while (executor.tryRunReadyTurn())
    {
    }

    CHECK_FALSE(renderLog.last().hasError);
    CHECK(renderLog.last().tooltip.empty());
  }

  TEST_CASE("TrackFilterViewModel - filter edits preserve focused view presentation", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    auto config = rt::TrackListViewConfig{.listId = rt::kAllTracksListId};
    config.optPresentation = rt::defaultTrackPresentationSpec();
    config.optPresentation->id = "custom";
    auto const viewId = ao::test::requireValue(fixture.workspaceService.navigate({
      .target = rt::GlobalViewKind::AllTracks,
      .optPresentation =
        rt::NavigationPresentation{
          .mode = rt::NavigationPresentationMode::Override,
          .spec = *config.optPresentation,
        },
    }));

    fixture.viewModel.updateFilter("$artist = \"Muse\"");

    auto const state = fixture.viewService.trackListState(viewId);
    CHECK(state.filterExpression == "$artist = \"Muse\"");
    CHECK(state.presentation.id == "custom");
    CHECK(fixture.renderLog.last().entryText == "$artist = \"Muse\"");
    CHECK(fixture.renderLog.last().resolvedExpression == "$artist = \"Muse\"");
  }

  TEST_CASE("TrackFilterViewModel - quoted plain text is escaped in quick search", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();

    fixture.viewModel.updateFilter("\"A Song Name\"");

    CHECK(fixture.renderLog.last().entryText == "\"A Song Name\"");
    CHECK(fixture.renderLog.last().resolvedExpression.contains("\"A Song Name\""));
    CHECK(fixture.renderLog.last().canCreateSmartList == true);
  }

  TEST_CASE("TrackFilterViewModel - losing focus clears filter state", "[uimodel][unit][track-filter]")
  {
    auto fixture = TrackFilterFixture{};
    fixture.focusAllTracksView();
    fixture.viewModel.updateFilter("Beatles");

    REQUIRE(fixture.workspaceService.closeView(fixture.workspaceService.snapshot().activeViewId));
    CHECK(fixture.renderLog.last().enabled == false);
    CHECK(fixture.renderLog.last().entryText.empty());
  }
} // namespace ao::uimodel::test
