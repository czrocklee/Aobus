// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackPageHost.h"

#include "ao/Type.h"
#include "ao/library/MusicLibrary.h"
#include "image/ImageCache.h"
#include "list/ListNavigationController.h"
#include "tag/TagEditController.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackPresentationStore.h"
#include "track/TrackRowCache.h"
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/playback/PlaybackQueueModel.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/stack.h>
#include <gtkmm/window.h>

#include <utility>

namespace ao::gtk::test
{
  TEST_CASE("TrackPageHost - lifecycle", "[gtk][track][host]")
  {
    [[maybe_unused]] auto const app = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& library = runtime.musicLibrary();
    auto cache = TrackRowCache{library};
    auto imageCache = ImageCache{200};
    auto window = Gtk::Window{};

    auto stack = Gtk::Stack{};
    auto tagEditCallbacks = TagEditController::Callbacks{};
    auto tagEditController = TagEditController{window, runtime, std::move(tagEditCallbacks)};

    auto sidebarCallbacks = ListNavigationController::Callbacks{};
    auto listSidebar = ListNavigationController{window, runtime, std::move(sidebarCallbacks)};

    auto presentationStore = TrackPresentationStore{runtime.workspace()};
    auto queueModel = uimodel::playback::PlaybackQueueModel{
      runtime.playback(), [&cache](TrackId id) { return cache.playbackDescriptor(id); }};

    auto host =
      TrackPageHost{stack, runtime, &queueModel, tagEditController, listSidebar, presentationStore, &imageCache};

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
