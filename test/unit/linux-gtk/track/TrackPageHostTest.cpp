// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPageHost.h"

#include "app/ThemeCoordinator.h"
#include "list/ListNavigationController.h"
#include "tag/TagEditController.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/source/TrackSourceCache.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/stack.h>
#include <gtkmm/window.h>

#include <cstddef>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("TrackPageHost - binds runtime pages to the GTK stack", "[gtk][integration][track]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto window = Gtk::Window{};

    auto stack = Gtk::Stack{};
    auto themeCoordinator = ThemeCoordinator{};
    auto tagEditCallbacks = TagEditController::Callbacks{};
    auto tagEditController = TagEditController{window,
                                               runtime.async(),
                                               runtime.library(),
                                               runtime.completion(),
                                               runtime.notifications(),
                                               runtime.textOrderingPolicy(),
                                               ao::test::englishMessageCatalog(),
                                               std::move(tagEditCallbacks),
                                               themeCoordinator};

    auto navCallbacks = ListNavigationController::Callbacks{};
    auto listNavigation = ListNavigationController{
      window, runtime, ao::test::englishMessageCatalog(), std::move(navCallbacks), themeCoordinator};

    auto columnLayouts = uimodel::TrackColumnLayouts{runtime.library().changes()};
    auto host = TrackPageHost{stack,
                              runtime.async(),
                              runtime.library(),
                              runtime.playback(),
                              runtime.views(),
                              runtime.workspace(),
                              tagEditController,
                              listNavigation,
                              columnLayouts,
                              ao::test::englishMessageCatalog(),
                              runtime.resourceBytes()};

    SECTION("initial state")
    {
      CHECK(host.currentVisible() == nullptr);
    }

    SECTION("rebuild creating pages")
    {
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
      drainGtkEvents();

      host.rebuild(cache);
      drainGtkEvents();

      auto const* const current = host.currentVisible();
      REQUIRE(current != nullptr);
      REQUIRE(current->pagePtr != nullptr);
      CHECK(current->viewId == viewId);
      CHECK(current->pagePtr->listId() == rt::kAllTracksListId);
      CHECK(host.activeListId() == rt::kAllTracksListId);
    }

    SECTION("rebuild restores the runtime selection into the new page generation")
    {
      auto const trackId = addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Restored selection"});
      runtime.sources().reloadAllTracks();
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));

      REQUIRE(runtime.views().setSelection(viewId, {trackId}));

      host.rebuild(cache);
      drainGtkEvents();

      auto* const context = host.find(viewId);
      REQUIRE(context != nullptr);
      REQUIRE(context->pagePtr != nullptr);
      CHECK(context->pagePtr->selectionController().selectedTrackIds() == std::vector<TrackId>{trackId});
      CHECK(runtime.views().trackListState(viewId).selection == std::vector<TrackId>{trackId});
    }

    SECTION("rebuild restores mixed selections without publishing or changing workspace focus")
    {
      auto const firstTrackId = addRuntimeTrack(runtime, library::test::TrackSpec{.title = "A restored track"});
      auto const secondTrackId = addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Z restored track"});
      auto const absentTrackId = TrackId{9999};
      runtime.sources().reloadAllTracks();
      auto const listId = ao::test::requireValue(
        runGtkTask(runtime, runtime.library().commands().createListAsync(rt::ListDraft{.name = "Other page"})));
      auto const firstViewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
      auto const secondViewId = ao::test::requireValue(runtime.workspace().navigate({.target = listId}));
      REQUIRE(firstViewId != secondViewId);
      auto const storedSelection = std::vector{firstTrackId, absentTrackId, secondTrackId};
      REQUIRE(runtime.views().setSelection(firstViewId, storedSelection));
      REQUIRE(runtime.views().setSelection(secondViewId, {secondTrackId}));
      REQUIRE(runtime.workspace().focusView(firstViewId));
      host.rebuild(cache);
      drainGtkEvents();

      REQUIRE(runtime.workspace().focusView(firstViewId));
      drainGtkEvents();
      auto const workspaceBefore = runtime.workspace().snapshot();
      std::size_t selectionChangeCount = 0;
      std::size_t workspaceChangeCount = 0;
      auto selectionSubscription =
        runtime.views().onSelectionChanged([&](rt::ViewService::SelectionChanged const&) { ++selectionChangeCount; });
      auto workspaceSubscription =
        runtime.workspace().onChanged([&](rt::WorkspaceChanged const&) { ++workspaceChangeCount; });

      host.rebuild(cache);
      drainGtkEvents();

      auto const* const firstPage = host.find(firstViewId);
      auto const* const secondPage = host.find(secondViewId);
      REQUIRE(firstPage != nullptr);
      REQUIRE(secondPage != nullptr);
      CHECK(firstPage->pagePtr->selectionController().selectedTrackIds() ==
            std::vector<TrackId>{firstTrackId, secondTrackId});
      CHECK(secondPage->pagePtr->selectionController().selectedTrackIds() == std::vector<TrackId>{secondTrackId});
      CHECK(runtime.views().trackListState(firstViewId).selection == storedSelection);
      CHECK(selectionChangeCount == 0);
      CHECK(workspaceChangeCount == 0);
      CHECK(runtime.workspace().snapshot().activeViewId == workspaceBefore.activeViewId);
      CHECK(runtime.workspace().snapshot().revision == workspaceBefore.revision);
    }

    SECTION("rebuild tolerates a selection naming a track absent from the projection")
    {
      auto const absentTrackId = TrackId{9999};
      auto const viewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));

      REQUIRE(runtime.views().setSelection(viewId, {absentTrackId}));

      host.rebuild(cache);
      drainGtkEvents();

      auto* const context = host.find(viewId);
      REQUIRE(context != nullptr);
      REQUIRE(context->pagePtr != nullptr);
      CHECK(context->pagePtr->selectionController().selectedTrackIds().empty());
      CHECK(runtime.views().trackListState(viewId).selection == std::vector<TrackId>{absentTrackId});
    }

    SECTION("group placeholder style reaches future and existing page generations")
    {
      host.setGroupCoverPlaceholderStyle(uimodel::CoverArtPlaceholderStyle::Vinyl);
      REQUIRE(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
      host.rebuild(cache);
      drainGtkEvents();

      auto* const context = host.currentVisible();
      REQUIRE(context != nullptr);
      REQUIRE(context->pagePtr != nullptr);
      CHECK(context->pagePtr->groupCoverPlaceholderStyle() == uimodel::CoverArtPlaceholderStyle::Vinyl);

      host.setGroupCoverPlaceholderStyle(uimodel::CoverArtPlaceholderStyle::Soul);
      CHECK(context->pagePtr->groupCoverPlaceholderStyle() == uimodel::CoverArtPlaceholderStyle::Soul);
    }

    SECTION("focus and close retire the exact page generation")
    {
      auto const listId = ao::test::requireValue(
        runGtkTask(runtime, runtime.library().commands().createListAsync(rt::ListDraft{.name = "Second page"})));
      auto const firstViewId = ao::test::requireValue(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
      auto const secondViewId = ao::test::requireValue(runtime.workspace().navigate({.target = listId}));
      REQUIRE(firstViewId != secondViewId);

      host.rebuild(cache);
      drainGtkEvents();
      REQUIRE(host.find(firstViewId) != nullptr);
      REQUIRE(host.find(secondViewId) != nullptr);
      REQUIRE(host.currentVisible() != nullptr);
      CHECK(host.currentVisible()->viewId == secondViewId);

      REQUIRE(runtime.workspace().focusView(firstViewId));
      drainGtkEvents();
      REQUIRE(host.currentVisible() != nullptr);
      CHECK(host.currentVisible()->viewId == firstViewId);

      REQUIRE(runtime.workspace().closeView(firstViewId));
      drainGtkEvents();
      CHECK(host.find(firstViewId) == nullptr);
      REQUIRE(host.find(secondViewId) != nullptr);
      REQUIRE(host.currentVisible() != nullptr);
      CHECK(host.currentVisible()->viewId == secondViewId);
      CHECK(runtime.workspace().snapshot().activeViewId == secondViewId);
    }

    SECTION("track activation starts from the owning view identity")
    {
      rt::test::addReadyAudioProvider(runtime);
      auto const trackId = addRuntimeTrack(
        runtime,
        library::test::TrackSpec{
          .title = "Activated", .uri = audio::test::requireAudioFixture("basic_metadata.flac").string()});
      runtime.sources().reloadAllTracks();
      REQUIRE(runtime.workspace().navigate({.target = rt::GlobalViewKind::AllTracks}));
      auto const viewId = runtime.workspace().snapshot().activeViewId;
      REQUIRE(viewId != rt::kInvalidViewId);

      host.rebuild(cache);
      drainGtkEvents();

      auto* const context = host.currentVisible();
      REQUIRE(context != nullptr);
      context->pagePtr->signalTrackActivated().emit(trackId);
      REQUIRE(tryWaitForPlaybackSettlement(runtime, trackId));

      auto const snapshot = runtime.playback().snapshot();
      CHECK(snapshot.succession.currentTrackId == trackId);
      CHECK(snapshot.succession.sourceListId == rt::kAllTracksListId);
      CHECK(snapshot.transport.nowPlaying.trackId == trackId);
    }

    SECTION("reveal synchronizes a missing workspace page before selecting the track")
    {
      auto const trackId = addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Reveal Target"});
      runtime.sources().reloadAllTracks();
      host.rebuild(cache);
      REQUIRE(host.currentVisible() == nullptr);

      auto const res = runtime.jumpToAlbum(trackId);

      REQUIRE(res);
      drainGtkEvents();
      auto const activeViewId = runtime.workspace().snapshot().activeViewId;
      REQUIRE(host.find(activeViewId) != nullptr);
      CHECK(runtime.views().trackListState(activeViewId).selection == std::vector<TrackId>{trackId});
    }
  }
} // namespace ao::gtk::test
