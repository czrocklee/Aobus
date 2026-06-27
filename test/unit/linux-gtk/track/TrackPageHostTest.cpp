// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPageHost.h"

#include "app/ThemeCoordinator.h"
#include "list/ListNavigationController.h"
#include "tag/TagEditController.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/library/MusicLibrary.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/playback/PlaybackQueueModel.h>
#include <ao/uimodel/track/TrackPresentationViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/stack.h>
#include <gtkmm/window.h>

#include <utility>

namespace ao::gtk::test
{
  TEST_CASE("TrackPageHost binds runtime pages to the GTK stack", "[gtk][unit][track][host]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& library = runtime.musicLibrary();
    auto cache = TrackRowCache{runtime.library()};
    auto window = Gtk::Window{};

    auto stack = Gtk::Stack{};
    auto themeController = ThemeCoordinator{};
    auto tagEditCallbacks = TagEditController::Callbacks{};
    auto tagEditController = TagEditController{window, runtime, std::move(tagEditCallbacks), themeController};

    auto navCallbacks = ListNavigationController::Callbacks{};
    auto listNavigation = ListNavigationController{window, runtime, std::move(navCallbacks), themeController};

    auto presentationStore = uimodel::track::TrackPresentationViewModel{runtime.workspace()};
    auto queueModel = uimodel::playback::PlaybackQueueModel{runtime.playback()};

    auto host = TrackPageHost{stack, runtime, &queueModel, tagEditController, listNavigation, presentationStore};

    SECTION("initial state")
    {
      CHECK(host.currentVisible() == nullptr);
    }

    SECTION("rebuild creating pages")
    {
      runtime.workspace().navigateTo(rt::kAllTracksListId);
      drainGtkEvents();

      auto txn = library.readTransaction();
      host.rebuild(cache, txn);
      drainGtkEvents();

      // Should have created a page for All Tracks
      CHECK(host.activeListId() == rt::kAllTracksListId);
    }
  }
} // namespace ao::gtk::test
