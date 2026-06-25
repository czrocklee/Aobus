// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackListModel.h"

#include "../../TestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/projection/TrackListProjection.h>

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
    struct TrackSpec final
    {
      std::string title = "Title";
      std::string artist = "Artist";
      std::string album = "Album";
      std::string albumArtist = "AlbumArtist";
      std::string genre = "Genre";
      std::uint16_t year = 2020;
      std::uint16_t trackNumber = 1;
      std::uint16_t discNumber = 1;
      std::chrono::milliseconds duration = std::chrono::minutes{3};
    };

    TrackSpec makeTrackSpec(std::string_view title,
                            std::string_view artist,
                            std::string_view album,
                            std::uint16_t year = 2020)
    {
      auto spec = TrackSpec{};
      spec.title = title;
      spec.artist = artist;
      spec.album = album;
      spec.year = year;
      return spec;
    }

    class TestMusicLibrary final
    {
    public:
      library::MusicLibrary& library() { return _fixture.runtime().musicLibrary(); }
      rt::AppRuntime& runtime() { return _fixture.runtime(); }

      TrackId addTrack(TrackSpec const& spec)
      {
        auto& lib = library();
        auto txn = lib.writeTransaction();
        auto writer = lib.tracks().writer(txn);

        auto builder = library::TrackBuilder::createNew();
        builder.metadata()
          .title(spec.title)
          .artist(spec.artist)
          .album(spec.album)
          .albumArtist(spec.albumArtist)
          .genre(spec.genre)
          .year(spec.year)
          .trackNumber(spec.trackNumber)
          .discNumber(spec.discNumber);
        builder.property()
          .uri("/tmp/test.flac")
          .duration(spec.duration)
          .bitrate(Bitrate{320000})
          .sampleRate(SampleRate{44100})
          .channels(Channels{2})
          .bitDepth(BitDepth{16});

        auto hotData = builder.serializeHot(txn, lib.dictionary());
        REQUIRE(hotData);
        auto coldData = builder.serializeCold(txn, lib.dictionary(), lib.resources());
        REQUIRE(coldData);
        auto [id, _] = ao::test::requireValue(writer.createHotCold(*hotData, *coldData));
        REQUIRE(txn.commit());
        return id;
      }

    private:
      GtkRuntimeFixture _fixture;
    };

    struct ModelSpy final
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

      void onItemsChanged(::guint pos, ::guint rem, ::guint add)
      {
        events.push_back({pos, rem, add, modelPtr->get_n_items()});
      }
    };
  } // namespace

  TEST_CASE("TrackListModel", "[app][unit][adapter]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.list_model_test");
    auto testLibrary = TestMusicLibrary{};

    auto const id1 = testLibrary.addTrack(makeTrackSpec("Song A", "Artist A", "Album A", 2020));
    auto const id2 = testLibrary.addTrack(makeTrackSpec("Song B", "Artist B", "Album B", 2021));

    auto source = MutableTrackSource{};
    source.addInitial(id1);
    source.addInitial(id2);

    auto rowCache = TrackRowCache{testLibrary.runtime().library()};
    auto const projectionPtr = std::make_shared<rt::TrackListProjection>(rt::ViewId{1}, source, testLibrary.library());

    auto const modelPtr = TrackListModel::create(rowCache);
    modelPtr->bindProjection(projectionPtr);

    auto spy = ModelSpy{};
    spy.modelPtr = modelPtr;
    modelPtr->signal_items_changed().connect(sigc::mem_fun(spy, &ModelSpy::onItemsChanged));

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
      auto const id3 = testLibrary.addTrack(makeTrackSpec("Song C", "Artist C", "Album C", 2022));
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
      source.onReset();

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
