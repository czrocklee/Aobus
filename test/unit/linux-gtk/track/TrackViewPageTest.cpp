// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackViewPage.h"

#include "../../TestUtils.h"
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
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/projection/LiveTrackListProjection.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/enums.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <vector>

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

    std::vector<TrackId> seedLargeProjection(library::MusicLibrary& library, std::size_t count)
    {
      auto transaction = library.writeTransaction();
      auto writer = library.tracks().writer(transaction);
      auto trackIds = std::vector<TrackId>{};
      trackIds.reserve(count);

      for (std::size_t index = 0; index < count; ++index)
      {
        auto builder = library::TrackBuilder::makeEmpty();
        auto const spec = library::test::TrackSpec{
          .title = std::format("Track {:05}", index),
          .artist = std::format("Artist {:03}", index % 100),
          .album = std::format("Album {:04}", index % 1000),
          .albumArtist = std::format("Album Artist {:03}", index % 100),
          .uri = std::format("/music/track_{}.flac", index),
          .duration = std::chrono::minutes{3},
        };
        library::test::applyTrackSpec(builder, spec);

        auto data = builder.serialize(transaction, library.resources());
        REQUIRE(data);
        trackIds.push_back(ao::test::requireValue(writer.createHotCold(data->first, data->second)).first);
      }

      REQUIRE(transaction.commit());
      return trackIds;
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
      auto sourcePtr = std::make_shared<rt::test::MutableTrackSource>();
      sourcePtr->addInitial(addAlbumTrack(runtime.musicLibrary(), "Album"));

      auto projectionPtr = std::make_shared<rt::LiveTrackListProjection>(
        rt::ViewId{1}, rt::TrackSourceLease{sourcePtr}, runtime.musicLibrary());
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

  TEST_CASE("TrackViewPage - large projections materialize only the GTK prefetch window",
            "[gtk][regression][track-view]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    constexpr std::size_t kTrackCount = 10000;
    constexpr std::size_t kMaximumPrefetchedRows = kTrackCount / 10;
    auto const trackIds = seedLargeProjection(runtime.musicLibrary(), kTrackCount);
    REQUIRE(trackIds.size() == kTrackCount);

    auto sourcePtr = rt::test::makeMutableTrackSource(trackIds);
    auto projectionPtr = std::make_shared<rt::LiveTrackListProjection>(
      rt::kInvalidViewId, rt::TrackSourceLease{sourcePtr}, runtime.musicLibrary());
    auto rowCache = TrackRowCache{runtime.library()};
    auto modelPtr = TrackListModel::create(rowCache);
    modelPtr->bindProjection(projectionPtr);
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ThumbnailLoader{runtime.library(), imageCache, runtime.async()};

    auto materializedRowsForPage = [&]
    {
      auto layoutStore = uimodel::TrackColumnLayoutStore{};
      auto page = TrackViewPage{rt::kAllTracksListId, modelPtr, layoutStore, runtime, thumbnailLoader};
      auto window = Gtk::Window{};
      window.set_child(page);
      window.set_default_size(600, 320);
      window.set_visible(true);
      drainGtkEvents();

      auto const materializedRows = rowCache.cachedRowCount();
      window.unset_child();
      drainGtkEvents();
      return materializedRows;
    };

    auto const ungroupedRows = materializedRowsForPage();
    CHECK(ungroupedRows > 0);
    CHECK(ungroupedRows < kMaximumPrefetchedRows);

    rowCache.clearCache();
    projectionPtr->setPresentation(rt::TrackPresentationSpec{
      .groupBy = rt::TrackGroupKey::Album,
      .sortBy = {{.field = rt::TrackSortField::Album}},
    });

    auto const groupedRows = materializedRowsForPage();
    CHECK(groupedRows > 0);
    CHECK(groupedRows < kMaximumPrefetchedRows);
  }
} // namespace ao::gtk::test
