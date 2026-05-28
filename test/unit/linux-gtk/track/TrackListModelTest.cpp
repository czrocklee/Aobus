// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackListModel.h"

#include "ao/Type.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackBuilder.h"
#include "ao/library/TrackStore.h"
#include "ao/lmdb/Transaction.h"
#include "ao/rt/TrackField.h"
#include "ao/rt/TrackListProjection.h"
#include "ao/rt/TrackSource.h"
#include "test/unit/lmdb/TestUtils.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"

#include <catch2/catch_test_macros.hpp>
#include <giomm/listmodel.h>
#include <glibmm/refptr.h>
#include <gtkmm/application.h>
#include <sigc++/functors/mem_fun.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    using namespace ao::lmdb::test;

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
      std::uint32_t durationMs = 180000;
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
      TestMusicLibrary()
        : _tempDir{}, _library{_tempDir.path(), _tempDir.path()}
      {
      }

      library::MusicLibrary& library() { return _library; }

      TrackId addTrack(TrackSpec const& spec)
      {
        auto txn = _library.writeTransaction();
        auto writer = _library.tracks().writer(txn);

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
          .durationMs(spec.durationMs)
          .bitrate(320000)
          .sampleRate(44100)
          .channels(2)
          .bitDepth(16);

        auto const hotData = builder.serializeHot(txn, _library.dictionary());
        auto const coldData = builder.serializeCold(txn, _library.dictionary(), _library.resources());
        auto [id, _] = writer.createHotCold(hotData, coldData);
        txn.commit();
        return id;
      }

    private:
      TempDir _tempDir;
      library::MusicLibrary _library;
    };

    class MutableTrackSource final : public rt::TrackSource
    {
    public:
      void addInitial(TrackId id) { _ids.push_back(id); }

      void insert(TrackId id, std::size_t index)
      {
        _ids.insert(_ids.begin() + static_cast<std::ptrdiff_t>(index), id);
        rt::TrackSource::notifyInserted(id, index);
      }

      void update(TrackId id)
      {
        auto const optIndex = indexOf(id);
        REQUIRE(optIndex.has_value());
        rt::TrackSource::notifyUpdated(id, *optIndex);
      }

      void onReset() { rt::TrackSource::notifyReset(); }

      void remove(TrackId id)
      {
        auto const optIndex = indexOf(id);
        REQUIRE(optIndex.has_value());
        _ids.erase(_ids.begin() + static_cast<std::ptrdiff_t>(*optIndex));
        rt::TrackSource::notifyRemoved(id, *optIndex);
      }

      std::size_t size() const override { return _ids.size(); }

      TrackId trackIdAt(std::size_t index) const override { return _ids.at(index); }

      std::optional<std::size_t> indexOf(TrackId id) const override
      {
        auto it = std::ranges::find(_ids, id);

        if (it == _ids.end())
        {
          return std::nullopt;
        }

        return static_cast<std::size_t>(std::ranges::distance(_ids.begin(), it));
      }

    private:
      std::vector<TrackId> _ids;
    };

    struct ModelSpy final
    {
      struct Event
      {
        ::guint position;
        ::guint removed;
        ::guint added;
      };

      std::vector<Event> events;

      void onItemsChanged(::guint pos, ::guint rem, ::guint add) { events.push_back({pos, rem, add}); }
    };
  } // namespace

  TEST_CASE("TrackListModel", "[app][unit][adapter]")
  {
    auto const app = Gtk::Application::create("io.github.aobus.list_model_test");
    auto testLibrary = TestMusicLibrary{};

    auto const id1 = testLibrary.addTrack(makeTrackSpec("Song A", "Artist A", "Album A", 2020));
    auto const id2 = testLibrary.addTrack(makeTrackSpec("Song B", "Artist B", "Album B", 2021));

    auto source = MutableTrackSource{};
    source.addInitial(id1);
    source.addInitial(id2);

    auto rowCache = TrackRowCache{testLibrary.library()};
    auto const projection = std::make_shared<rt::TrackListProjection>(rt::ViewId{1}, source, testLibrary.library());

    auto const model = TrackListModel::create(rowCache);
    model->bindProjection(projection);

    auto spy = ModelSpy{};
    model->signal_items_changed().connect(sigc::mem_fun(spy, &ModelSpy::onItemsChanged));

    SECTION("Basic properties and size")
    {
      CHECK(model->projection() == projection.get());
      CHECK(model->indexOf(id1) == 0);
      CHECK(model->indexOf(id2) == 1);
      CHECK_FALSE(model->groupIndexForTrack(id1).has_value());
      CHECK(model->get_n_items() == 2);
      CHECK(model->get_item_type() != G_TYPE_INVALID);

      auto const item = model->get_object(0);
      REQUIRE(item != nullptr);
      auto const castRow = std::dynamic_pointer_cast<TrackRowObject>(item);
      REQUIRE(castRow);
      CHECK(castRow->getTrackId() == id1);
      CHECK(castRow->getFieldText(rt::TrackField::Artist) == "Artist A");
    }

    SECTION("Playing state updates refresh model items")
    {
      CHECK(spy.events.empty());
      model->setPlayingTrackId(id1);

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].position == 0);
      CHECK(spy.events[0].removed == 1);
      CHECK(spy.events[0].added == 1);
    }

    SECTION("Delta batch notifications - Insert")
    {
      auto const id3 = testLibrary.addTrack(makeTrackSpec("Song C", "Artist C", "Album C", 2022));
      source.insert(id3, 0);

      CHECK(model->get_n_items() == 3);
      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].position == 2);
      CHECK(spy.events[0].removed == 0);
      CHECK(spy.events[0].added == 1);
    }

    SECTION("Delta batch notifications - Remove")
    {
      source.remove(id1);

      CHECK(model->get_n_items() == 1);
      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].position == 0);
      CHECK(spy.events[0].removed == 1);
      CHECK(spy.events[0].added == 0);
    }

    SECTION("Delta batch notifications - Update")
    {
      source.update(id2);

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].position == 1);
      CHECK(spy.events[0].removed == 1);
      CHECK(spy.events[0].added == 1);
    }

    SECTION("Delta batch notifications - Reset")
    {
      source.onReset();

      REQUIRE(spy.events.size() == 1);
      CHECK(spy.events[0].position == 0);
      CHECK(spy.events[0].removed == 2);
      CHECK(spy.events[0].added == 2);
    }

    SECTION("Clearing and unbinding projection")
    {
      model->clearProjection();
      CHECK(model->projection() == nullptr);
      CHECK(model->get_n_items() == 0);
    }
  }
} // namespace ao::gtk::test
