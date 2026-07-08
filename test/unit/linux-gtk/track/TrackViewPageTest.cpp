// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackViewPage.h"

#include "image/ImageCache.h"
#include "image/ThumbnailLoader.h"
#include "layout/LayoutConstants.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/enums.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace ao::gtk::test
{
  namespace
  {
    TrackId addAlbumTrack(library::MusicLibrary& library, std::string const& album)
    {
      return library::test::addTrack(library,
                                     library::test::TrackSpec{.title = "Track",
                                                              .artist = "Artist",
                                                              .album = album,
                                                              .albumArtist = "Album Artist",
                                                              .uri = "/tmp/track.flac",
                                                              .trackNumber = 1,
                                                              .duration = std::chrono::minutes{3}});
    }
  } // namespace

  TEST_CASE("TrackViewPage - initializes list controls and geometry", "[gtk][unit][geometry]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library()};
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ThumbnailLoader{runtime.library(), imageCache, runtime.async()};
    auto window = Gtk::Window{};

    auto modelPtr = TrackListModel::create(cache);
    auto layoutStore = uimodel::TrackColumnLayoutStore{};

    auto page = TrackViewPage{rt::kAllTracksListId, modelPtr, layoutStore, runtime, thumbnailLoader};
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

    SECTION("album grouped section header reserves a fixed cover slot")
    {
      auto source = rt::test::MutableTrackSource{};
      source.addInitial(addAlbumTrack(runtime.musicLibrary(), "Album"));

      auto projectionPtr = std::make_shared<rt::TrackListProjection>(rt::ViewId{1}, source, runtime.musicLibrary());
      auto presentation = rt::TrackPresentationSpec{.groupBy = rt::TrackGroupKey::Album};
      projectionPtr->setPresentation(presentation);
      modelPtr->bindProjection(projectionPtr);
      page.applyPresentation(projectionPtr->presentation());

      window.set_default_size(600, 320);
      window.set_visible(true);
      drainGtkEvents();

      auto* const coverSlot = findWidgetByClass<Gtk::Widget>(page, "ao-track-section-cover");
      REQUIRE(coverSlot != nullptr);
      CHECK(coverSlot->get_visible());

      std::int32_t minSize = {};
      std::int32_t natSizeHoriz = {};
      std::int32_t natSizeVert = {};
      std::int32_t minBaseline = {};
      std::int32_t natBaseline = {};
      coverSlot->measure(Gtk::Orientation::HORIZONTAL, -1, minSize, natSizeHoriz, minBaseline, natBaseline);
      coverSlot->measure(Gtk::Orientation::VERTICAL, -1, minSize, natSizeVert, minBaseline, natBaseline);

      CHECK(natSizeHoriz == natSizeVert);
      CHECK(natSizeHoriz >= layout::kSectionCoverLogicalSize);
      CHECK(natSizeHoriz <= layout::kSectionCoverLogicalSize + 2);
    }
  }
} // namespace ao::gtk::test
