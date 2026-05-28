// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackViewPage.h"

#include "image/ImageCache.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackListModel.h"
#include "track/TrackPresentationStore.h"
#include "track/TrackRowCache.h"
#include <ao/rt/CorePrimitives.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/window.h>

namespace ao::gtk::test
{
  TEST_CASE("TrackViewPage - initialization", "[gtk][track][page]")
  {
    [[maybe_unused]] auto const app = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& library = runtime.musicLibrary();
    auto cache = TrackRowCache{library};
    auto imageCache = ImageCache{200};
    auto window = Gtk::Window{};

    auto model = TrackListModel::create(cache);
    auto presentationStore = TrackPresentationStore{runtime.workspace()};

    auto page = TrackViewPage{rt::kAllTracksListId, model, presentationStore, runtime, imageCache};
    window.set_child(page);

    SECTION("initial state")
    {
      CHECK(page.listId() == rt::kAllTracksListId);
      CHECK(page.projection() == nullptr);
    }

    SECTION("set playing track doesn't crash")
    {
      page.setPlayingTrackId(TrackId{1});
    }

    SECTION("status message management")
    {
      page.setStatusMessage("Loading...");
      page.clearStatusMessage();
    }
  }
} // namespace ao::gtk::test
