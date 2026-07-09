// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackListModel.h"

#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/runtime/source/TrackSourceTestSupport.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/projection/LiveTrackListProjection.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/listmodel.h>
#include <glibmm/refptr.h>
#include <gtkmm/application.h>
#include <sigc++/functors/mem_fun.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    library::test::TrackSpec makeTrackSpec(std::string_view title,
                                           std::string_view artist,
                                           std::string_view album,
                                           std::uint16_t year = 2020)
    {
      auto spec = library::test::TrackSpec{};
      spec.title = title;
      spec.artist = artist;
      spec.album = album;
      spec.albumArtist = "AlbumArtist";
      spec.genre = "Genre";
      spec.year = year;
      spec.duration = std::chrono::minutes{3};
      return spec;
    }

    struct SpyTrackListModelEvents final
    {
      struct Event
      {
        ::guint position;
        ::guint removed;
        ::guint added;
        ::guint sizeDuringEvent;
      };

      std::vector<Event> events;
      Glib::RefPtr<TrackListModel> modelPtr;

      void handleItemsChanged(::guint position, ::guint removed, ::guint added)
      {
        events.push_back({position, removed, added, modelPtr->get_n_items()});
      }
    };
  } // namespace

  TEST_CASE("TrackListModel - exposes projection rows and emits playing-track updates", "[gtk][unit][track][adapter]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.list_model_test");
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& musicLibrary = runtime.musicLibrary();

    auto const id1 = library::test::addTrack(musicLibrary, makeTrackSpec("Song A", "Artist A", "Album A", 2020));
    auto const id2 = library::test::addTrack(musicLibrary, makeTrackSpec("Song B", "Artist B", "Album B", 2021));

    auto source = rt::test::MutableTrackSource{};
    source.addInitial(id1);
    source.addInitial(id2);

    auto rowCache = TrackRowCache{runtime.library()};
    auto const projectionPtr = std::make_shared<rt::LiveTrackListProjection>(rt::ViewId{1}, source, musicLibrary);

    auto const modelPtr = TrackListModel::create(rowCache);
    modelPtr->bindProjection(projectionPtr);

    auto spy = SpyTrackListModelEvents{};
    spy.modelPtr = modelPtr;
    modelPtr->signal_items_changed().connect(sigc::mem_fun(spy, &SpyTrackListModelEvents::handleItemsChanged));

    SECTION("Basic properties and size")
    {
      CHECK(modelPtr->projection() == projectionPtr.get());
      CHECK(modelPtr->indexOf(id1) == 0);
      CHECK(modelPtr->indexOf(id2) == 1);
      CHECK_FALSE(modelPtr->groupIndexForTrack(id1).has_value());
      CHECK(modelPtr->get_n_items() == 2);
      CHECK(modelPtr->get_item_type() != G_TYPE_INVALID);

      auto const itemPtr = modelPtr->get_object(0);
      REQUIRE(itemPtr != nullptr);
      auto const castRowPtr = std::dynamic_pointer_cast<TrackRowObject>(itemPtr);
      REQUIRE(castRowPtr);
      CHECK(castRowPtr->trackId() == id1);
      CHECK(castRowPtr->fieldText(rt::TrackField::Artist) == "Artist A");
    }

    SECTION("Setting the playing track emits the playing-changed signal, not items_changed")
    {
      std::int32_t playingChangedCount = 0;
      modelPtr->signalPlayingChanged().connect([&] { ++playingChangedCount; });

      CHECK(spy.events.empty());
      modelPtr->setPlayingTrackId(id1);

      // The shared, cached row objects make items_changed a no-op (GTK dedups the
      // rebind), so the highlight is driven by the dedicated signal instead.
      CHECK(spy.events.empty());
      CHECK(playingChangedCount == 1);
      CHECK(modelPtr->playingTrackId() == id1);

      // get_item_vfunc still stamps isPlaying() on the object it hands back, so a
      // freshly bound (scrolled-in) row reflects the current playing track.
      auto const playingRowPtr = std::dynamic_pointer_cast<TrackRowObject>(modelPtr->get_object(0));
      REQUIRE(playingRowPtr);
      CHECK(playingRowPtr->isPlaying());
    }

    SECTION("Setting the same playing track twice is a no-op")
    {
      std::int32_t playingChangedCount = 0;
      modelPtr->signalPlayingChanged().connect([&] { ++playingChangedCount; });

      modelPtr->setPlayingTrackId(id1);
      modelPtr->setPlayingTrackId(id1);

      CHECK(playingChangedCount == 1);
    }

    SECTION("Setting playing track outside the projection emits only the playing signal")
    {
      std::int32_t playingChangedCount = 0;
      modelPtr->signalPlayingChanged().connect([&] { ++playingChangedCount; });

      modelPtr->setPlayingTrackId(TrackId{987654});

      CHECK(playingChangedCount == 1);
      CHECK(spy.events.empty());
      CHECK(modelPtr->get_n_items() == 2);
      CHECK(modelPtr->playingTrackId() == TrackId{987654});
    }

    SECTION("Setting playing track before binding a projection records state and emits the signal")
    {
      auto emptyModelPtr = TrackListModel::create(rowCache);
      std::int32_t playingChangedCount = 0;
      emptyModelPtr->signalPlayingChanged().connect([&] { ++playingChangedCount; });

      emptyModelPtr->setPlayingTrackId(id1);

      CHECK(playingChangedCount == 1);
      CHECK(emptyModelPtr->playingTrackId() == id1);
      CHECK(emptyModelPtr->get_n_items() == 0);
    }

    SECTION("Switching the playing track re-emits the signal and restamps both rows")
    {
      std::int32_t playingChangedCount = 0;
      modelPtr->signalPlayingChanged().connect([&] { ++playingChangedCount; });

      modelPtr->setPlayingTrackId(id1);
      modelPtr->setPlayingTrackId(id2);

      CHECK(playingChangedCount == 2);
      CHECK(spy.events.empty());
      CHECK(modelPtr->playingTrackId() == id2);

      auto const oldRowPtr = std::dynamic_pointer_cast<TrackRowObject>(modelPtr->get_object(0));
      auto const newRowPtr = std::dynamic_pointer_cast<TrackRowObject>(modelPtr->get_object(1));
      REQUIRE(oldRowPtr);
      REQUIRE(newRowPtr);
      CHECK_FALSE(oldRowPtr->isPlaying());
      CHECK(newRowPtr->isPlaying());
    }

    SECTION("Delta batch notifications - Insert")
    {
      auto const id3 = library::test::addTrack(musicLibrary, makeTrackSpec("Song C", "Artist C", "Album C", 2022));
      source.insert(id3, 0);

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].position == 2);
      CHECK(spy.events[0].removed == 0);
      CHECK(spy.events[0].added == 1);
      CHECK(spy.events[0].sizeDuringEvent == 3);
      CHECK(modelPtr->get_n_items() == 3);
    }

    SECTION("Delta batch notifications - Remove")
    {
      source.remove(id1);

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].position == 0);
      CHECK(spy.events[0].removed == 1);
      CHECK(spy.events[0].added == 0);
      CHECK(spy.events[0].sizeDuringEvent == 1);
      CHECK(modelPtr->get_n_items() == 1);
    }

    SECTION("Delta batch notifications - Update")
    {
      source.update(id2);

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].position == 1);
      CHECK(spy.events[0].removed == 1);
      CHECK(spy.events[0].added == 1);
      CHECK(spy.events[0].sizeDuringEvent == 2);
    }

    SECTION("Delta batch notifications - Reset")
    {
      source.emitReset();

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].position == 0);
      CHECK(spy.events[0].removed == 2);
      CHECK(spy.events[0].added == 2);
      CHECK(spy.events[0].sizeDuringEvent == 2);
    }

    SECTION("Clearing and unbinding projection")
    {
      modelPtr->clearProjection();
      CHECK(modelPtr->projection() == nullptr);
      CHECK(modelPtr->get_n_items() == 0);
    }
  }
} // namespace ao::gtk::test
