// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackViewPage.h"

#include "image/ImageCache.h"
#include "image/ThumbnailLoader.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include <ao/rt/CorePrimitives.h>
#include <ao/uimodel/track/TrackPresentationViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/window.h>

namespace ao::gtk::test
{
  TEST_CASE("TrackViewPage - initialization", "[gtk][track][page]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& library = runtime.musicLibrary();
    auto cache = TrackRowCache{library};
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ThumbnailLoader{library, imageCache, runtime.async()};
    auto window = Gtk::Window{};

    auto modelPtr = TrackListModel::create(cache);
    auto presentationStore = uimodel::track::TrackPresentationViewModel{runtime.workspace()};

    auto page = TrackViewPage{rt::kAllTracksListId, modelPtr, presentationStore, runtime, thumbnailLoader};
    window.set_child(page);

    SECTION("initial state")
    {
      CHECK(page.listId() == rt::kAllTracksListId);
      CHECK(page.projection() == nullptr);
    }

    SECTION("status message shows then hides the status label")
    {
      page.setStatusMessage("Loading...");
      auto* const label = findLabelByText(page, "Loading...");
      REQUIRE(label != nullptr);
      CHECK(label->get_visible());

      page.clearStatusMessage();
      CHECK_FALSE(label->get_visible());
    }
  }
} // namespace ao::gtk::test
