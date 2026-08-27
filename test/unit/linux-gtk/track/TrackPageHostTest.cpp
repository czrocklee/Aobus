// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPageHost.h"

#include "app/ThemeCoordinator.h"
#include "list/ListNavigationController.h"
#include "tag/TagEditController.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/resource/ResourceByteLoader.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/stack.h>
#include <gtkmm/window.h>

#include <utility>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("TrackPageHost - binds runtime pages to the GTK stack", "[gtk][unit][track][host]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto window = Gtk::Window{};

    auto stack = Gtk::Stack{};
    auto themeCoordinator = ThemeCoordinator{};
    auto tagEditCallbacks = TagEditController::Callbacks{};
    auto tagEditController = TagEditController{
      window, runtime, ao::test::englishMessageCatalog(), std::move(tagEditCallbacks), themeCoordinator};

    auto navCallbacks = ListNavigationController::Callbacks{};
    auto listNavigation = ListNavigationController{
      window, runtime, ao::test::englishMessageCatalog(), std::move(navCallbacks), themeCoordinator};

    auto layoutStore = uimodel::TrackColumnLayoutStore{};
    auto byteLoader = rt::ResourceByteLoader{runtime};
    auto host = TrackPageHost{
      stack, runtime, tagEditController, listNavigation, layoutStore, ao::test::englishMessageCatalog(), byteLoader};

    SECTION("initial state")
    {
      CHECK(host.currentVisible() == nullptr);
    }

    SECTION("rebuild creating pages")
    {
      REQUIRE(runtime.workspace().navigate({.target = rt::kAllTracksListId}));
      drainGtkEvents();

      host.rebuild(cache);
      drainGtkEvents();

      // Should have created a page for All Tracks
      CHECK(host.activeListId() == rt::kAllTracksListId);
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

    SECTION("track activation starts from the owning view identity")
    {
      rt::test::addReadyAudioProvider(runtime);
      auto const trackId = addRuntimeTrack(
        runtime,
        library::test::TrackSpec{
          .title = "Activated", .uri = audio::test::requireAudioFixture("basic_metadata.flac").string()});
      runtime.reloadAllTracks();
      REQUIRE(runtime.workspace().navigate({.target = rt::GlobalViewKind::AllTracks}));
      auto const viewId = runtime.workspace().snapshot().activeViewId;
      REQUIRE(viewId != rt::kInvalidViewId);

      host.rebuild(cache);
      drainGtkEvents();

      auto* const context = host.currentVisible();
      REQUIRE(context != nullptr);
      context->pagePtr->signalTrackActivated().emit(trackId);
      REQUIRE(waitForPlaybackSettlement(runtime, trackId));

      auto const snapshot = runtime.playback().snapshot();
      CHECK(snapshot.succession.currentTrackId == trackId);
      CHECK(snapshot.succession.sourceListId == rt::kAllTracksListId);
      CHECK(snapshot.transport.nowPlaying.trackId == trackId);
    }

    SECTION("reveal synchronizes a missing workspace page before selecting the track")
    {
      auto const trackId = addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Reveal Target"});
      runtime.reloadAllTracks();
      host.rebuild(cache);
      REQUIRE(host.currentVisible() == nullptr);

      auto const result = runtime.jumpToAlbum(trackId);

      REQUIRE(result);
      drainGtkEvents();
      auto const activeViewId = runtime.workspace().snapshot().activeViewId;
      REQUIRE(host.find(activeViewId) != nullptr);
      CHECK(runtime.views().trackListState(activeViewId).selection == std::vector<TrackId>{trackId});
    }
  }
} // namespace ao::gtk::test
