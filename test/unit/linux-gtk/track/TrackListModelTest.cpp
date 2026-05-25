// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

// Standalone test for app::uigtk::TrackListModel without GTKMM dependency.
// Tests adapter functionality with test doubles for GTK objects.

#include "ao/Type.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackBuilder.h"
#include "ao/library/TrackStore.h"
#include "ao/lmdb/Transaction.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/rt/TrackSource.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ao::model::test
{
  // Test-specific RowData definition
  struct RowData final
  {
    TrackId id;
    std::string artist;
    std::string album;
    std::string albumArtist;
    std::string genre;
    std::string title;
    std::string tags;
    std::uint16_t year = 0;
    std::uint16_t discNumber = 0;
    std::uint16_t trackNumber = 0;
    std::optional<std::uint32_t> optCoverArtId;
    bool missing = false;
  };

  /**
   * TestTrackRowProvider - Standalone test double for app::uigtk::TrackListModel tests.
   * Named differently to avoid symbol collision when linking.
   */
  class TestTrackRowProvider final
  {
  public:
    explicit TestTrackRowProvider(library::MusicLibrary& ml);

    std::optional<RowData> getRow(TrackId id);
    void invalidateHot(TrackId id);
    void invalidateFull(TrackId id);
    void remove(TrackId id);

  private:
    std::string resolveDictionaryString(DictionaryId id);

    library::MusicLibrary* _ml;
    library::TrackStore* _store;
    library::DictionaryStore* _dict;

    std::unordered_map<TrackId, RowData> _rowCache;
    std::unordered_map<DictionaryId, std::string> _stringCache;
  };

  std::string TestTrackRowProvider::resolveDictionaryString(DictionaryId id)
  {
    if (auto it = _stringCache.find(id); it != _stringCache.end())
    {
      return it->second;
    }

    auto result = std::string{};

    try
    {
      auto const str = _dict->get(id);
      result = std::string{str};
    }
    catch (std::exception const&)
    {
      result = {};
    }

    auto const insertResult = _stringCache.emplace(id, result);

    return insertResult.first->second;
  }

  TestTrackRowProvider::TestTrackRowProvider(library::MusicLibrary& ml)
    : _ml{&ml}, _store{&ml.tracks()}, _dict{&ml.dictionary()}
  {
  }

  std::optional<RowData> TestTrackRowProvider::getRow(TrackId id)
  {
    if (auto it = _rowCache.find(id); it != _rowCache.end())
    {
      if (it->second.missing)
      {
        return std::nullopt;
      }

      return it->second;
    }

    lmdb::ReadTransaction const txn(_ml->readTransaction());
    auto reader = _store->reader(txn);

    if (auto const optView = reader.get(id, library::TrackStore::Reader::LoadMode::Both))
    {
      auto const& view = *optView;
      auto const& metadata = view.metadata();

      auto row = RowData{};
      row.id = id;
      row.title = std::string{metadata.title()};

      if (auto const artistId = metadata.artistId(); artistId != kInvalidDictionaryId)
      {
        row.artist = resolveDictionaryString(artistId);
      }

      if (auto const albumId = metadata.albumId(); albumId != kInvalidDictionaryId)
      {
        row.album = resolveDictionaryString(albumId);
      }

      if (auto const albumArtistId = metadata.albumArtistId(); albumArtistId != kInvalidDictionaryId)
      {
        row.albumArtist = resolveDictionaryString(albumArtistId);
      }

      if (auto const genreId = metadata.genreId(); genreId != kInvalidDictionaryId)
      {
        row.genre = resolveDictionaryString(genreId);
      }

      row.year = metadata.year();
      row.discNumber = metadata.discNumber();
      row.trackNumber = metadata.trackNumber();

      if (auto const coverArtId = metadata.coverArtId(); coverArtId != 0)
      {
        row.optCoverArtId = coverArtId;
      }

      auto const result = _rowCache.emplace(id, std::move(row));

      return result.first->second;
    }

    auto row = RowData{};
    row.id = id;
    row.missing = true;
    _rowCache.emplace(id, std::move(row));

    return std::nullopt;
  }

  void TestTrackRowProvider::invalidateHot(TrackId id)
  {
    _rowCache.erase(id);
  }

  void TestTrackRowProvider::invalidateFull(TrackId id)
  {
    _rowCache.erase(id);
  }

  void TestTrackRowProvider::remove(TrackId id)
  {
    _rowCache.erase(id);
  }
} // namespace ao::model::test

namespace ao::gtk::test
{
  namespace
  {
    using namespace ao::library;
    using namespace ao::lmdb::test;
    using namespace ao::model::test;
    using namespace ao::rt;

    struct TrackSpec final
    {
      std::string title = "Track";
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

      MusicLibrary& library() { return _library; }

      TrackId addTrack(TrackSpec const& spec)
      {
        auto txn = _library.writeTransaction();
        auto writer = _library.tracks().writer(txn);

        auto builder = TrackBuilder::createNew();
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
      MusicLibrary _library;
    };

    /**
     * MutableTrackSource - Test helper for TrackSource.
     */
    class MutableTrackSource final : public TrackSource
    {
    public:
      void addInitial(TrackId id) { _ids.push_back(id); }

      void insert(TrackId id, std::size_t index)
      {
        _ids.insert(_ids.begin() + static_cast<std::ptrdiff_t>(index), id);
        TrackSource::notifyInserted(id, index);
      }

      void update(TrackId id)
      {
        auto const optIndex = indexOf(id);
        REQUIRE(optIndex.has_value());
        TrackSource::notifyUpdated(id, *optIndex);
      }

      void onReset() { TrackSource::notifyReset(); }

      void remove(TrackId id)
      {
        auto const optIndex = indexOf(id);
        REQUIRE(optIndex.has_value());
        _ids.erase(_ids.begin() + static_cast<std::ptrdiff_t>(*optIndex));
        TrackSource::notifyRemoved(id, *optIndex);
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

      std::vector<TrackId> const& ids() const { return _ids; }

    private:
      std::vector<TrackId> _ids;
    };

    /**
     * ObserverSpy - Records observer events for verification.
     */
    struct ObserverSpy final : public TrackSourceObserver
    {
    public:
      enum class EventKind : std::uint8_t
      {
        Reset,
        Inserted,
        Updated,
        Removed,
      };

      struct Event
      {
        EventKind kind;
        TrackId id{};
        std::size_t index = 0;
      };

      void onReset() override { events.push_back({.kind = EventKind::Reset}); }

      void onInserted(TrackId id, std::size_t index) override
      {
        events.push_back({.kind = EventKind::Inserted, .id = id, .index = index});
      }

      void onUpdated(TrackId id, std::size_t index) override
      {
        events.push_back({.kind = EventKind::Updated, .id = id, .index = index});
      }

      void onRemoved(TrackId id, std::size_t index) override
      {
        events.push_back({.kind = EventKind::Removed, .id = id, .index = index});
      }

      void clear() { events.clear(); }

      std::vector<Event> events;
    };

    /**
     * Filter matcher - standalone version matching app::uigtk::TrackListModel logic.
     */
    bool matchesFilter(RowData const& rowData, std::string_view filter)
    {
      if (filter.empty())
      {
        return true;
      }

      auto needle = std::string{filter};
      std::ranges::transform(
        needle, needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

      auto const check = [&needle](std::string field) -> bool
      {
        if (field.empty())
        {
          return false;
        }

        std::ranges::transform(
          field, field.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        return field.find(needle) != std::string::npos;
      };

      return check(rowData.artist) || check(rowData.album) || check(rowData.albumArtist) || check(rowData.genre) ||
             check(rowData.title) || check(rowData.tags);
    }
  } // namespace

  using namespace ao::lmdb::test;

  TEST_CASE("app::uigtk::TrackListModel", "[app][adapter]")
  {
    SECTION("TestTrackRowProvider loads row data for filtering")
    {
      auto testLibrary = TestMusicLibrary{};
      auto provider = std::make_shared<TestTrackRowProvider>(testLibrary.library());
      auto const trackId = testLibrary.addTrack(makeTrackSpec("My Song", "Beatles", "Abbey Road", 2023));

      auto const optRow = provider->getRow(trackId);
      REQUIRE(optRow.has_value());
      CHECK(optRow->title == "My Song");
      CHECK(optRow->artist == "Beatles");
      CHECK(optRow->album == "Abbey Road");
      CHECK(optRow->year == 2023);
    }

    SECTION("filter matcher works for artist")
    {
      auto testLibrary = TestMusicLibrary{};
      auto provider = std::make_shared<TestTrackRowProvider>(testLibrary.library());
      auto const trackId = testLibrary.addTrack(makeTrackSpec("Song", "Beatles", "Album", 2020));

      auto const optRow = provider->getRow(trackId);
      REQUIRE(optRow.has_value());

      CHECK(matchesFilter(*optRow, "beatles") == true);
      CHECK(matchesFilter(*optRow, "stones") == false);
    }

    SECTION("filter matcher works for album")
    {
      auto testLibrary = TestMusicLibrary{};
      auto provider = std::make_shared<TestTrackRowProvider>(testLibrary.library());
      auto const trackId = testLibrary.addTrack(makeTrackSpec("Song", "Artist", "Abbey Road", 2020));

      auto const optRow = provider->getRow(trackId);
      REQUIRE(optRow.has_value());

      CHECK(matchesFilter(*optRow, "abbey") == true);
      CHECK(matchesFilter(*optRow, "revolver") == false);
    }

    SECTION("filter matcher works for title")
    {
      auto testLibrary = TestMusicLibrary{};
      auto provider = std::make_shared<TestTrackRowProvider>(testLibrary.library());
      auto const trackId = testLibrary.addTrack(makeTrackSpec("Yesterday", "Artist", "Album", 2020));

      auto const optRow = provider->getRow(trackId);
      REQUIRE(optRow.has_value());

      CHECK(matchesFilter(*optRow, "yesterday") == true);
      CHECK(matchesFilter(*optRow, "today") == false);
    }

    SECTION("filter matcher is case-insensitive")
    {
      auto testLibrary = TestMusicLibrary{};
      auto provider = std::make_shared<TestTrackRowProvider>(testLibrary.library());
      auto const trackId = testLibrary.addTrack(makeTrackSpec("Song", "The Beatles", "Album", 2020));

      auto const optRow = provider->getRow(trackId);
      REQUIRE(optRow.has_value());

      CHECK(matchesFilter(*optRow, "BEATLES") == true);
      CHECK(matchesFilter(*optRow, "beatles") == true);
      CHECK(matchesFilter(*optRow, "BeAtLeS") == true);
    }

    SECTION("filter matcher returns true for empty filter")
    {
      auto testLibrary = TestMusicLibrary{};
      auto provider = std::make_shared<TestTrackRowProvider>(testLibrary.library());
      auto const trackId = testLibrary.addTrack(makeTrackSpec("Song", "Artist", "Album", 2020));

      auto const optRow = provider->getRow(trackId);
      REQUIRE(optRow.has_value());

      CHECK(matchesFilter(*optRow, "") == true);
    }

    SECTION("MutableTrackSource notifies observers on insert")
    {
      auto source = MutableTrackSource{};
      auto spy = ObserverSpy{};
      source.attach(&spy);

      auto const id = TrackId{1};
      source.addInitial(id);

      CHECK(spy.events.empty()); // addInitial doesn't notify

      source.insert(TrackId{2}, 0);
      CHECK(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Inserted);
      CHECK(spy.events[0].id == TrackId{2});

      source.detach(&spy);
    }

    SECTION("MutableTrackSource notifies observers on remove")
    {
      auto source = MutableTrackSource{};
      auto const id1 = TrackId{1};
      auto const id2 = TrackId{2};
      source.addInitial(id1);
      source.addInitial(id2);

      auto spy = ObserverSpy{};
      source.attach(&spy);

      source.remove(id1);
      CHECK(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Removed);
      CHECK(spy.events[0].id == id1);

      source.detach(&spy);
    }

    SECTION("MutableTrackSource notifies observers on update")
    {
      auto source = MutableTrackSource{};
      auto const id = TrackId{1};
      source.addInitial(id);

      auto spy = ObserverSpy{};
      source.attach(&spy);

      source.update(id);
      CHECK(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Updated);
      CHECK(spy.events[0].id == id);

      source.detach(&spy);
    }

    SECTION("MutableTrackSource notifies observers on reset")
    {
      auto source = MutableTrackSource{};
      auto spy = ObserverSpy{};
      source.attach(&spy);

      source.onReset();
      CHECK(spy.events.size() == 1);
      CHECK(spy.events[0].kind == ObserverSpy::EventKind::Reset);

      source.detach(&spy);
    }

    SECTION("TestTrackRowProvider cache invalidation works")
    {
      auto testLibrary = TestMusicLibrary{};
      auto provider = std::make_shared<TestTrackRowProvider>(testLibrary.library());
      auto const trackId = testLibrary.addTrack(makeTrackSpec("Cache Test", "Artist", "Album", 2020));

      // First load
      auto const optFirst = provider->getRow(trackId);
      REQUIRE(optFirst.has_value());

      // Invalidate
      provider->invalidateHot(trackId);

      // Should reload
      auto const optSecond = provider->getRow(trackId);
      REQUIRE(optSecond.has_value());
      CHECK(optSecond->title == "Cache Test");
    }

    SECTION("multiple tracks can be loaded and filtered")
    {
      auto testLibrary = TestMusicLibrary{};
      auto provider = std::make_shared<TestTrackRowProvider>(testLibrary.library());

      auto const track1 = testLibrary.addTrack(makeTrackSpec("Song A", "Beatles", "Album 1", 2020));
      auto const track2 = testLibrary.addTrack(makeTrackSpec("Song B", "Stones", "Album 2", 2021));
      auto const track3 = testLibrary.addTrack(makeTrackSpec("Song C", "Beatles", "Album 3", 2022));

      auto const optRow1 = provider->getRow(track1);
      auto const optRow2 = provider->getRow(track2);
      auto const optRow3 = provider->getRow(track3);

      REQUIRE(optRow1.has_value());
      REQUIRE(optRow2.has_value());
      REQUIRE(optRow3.has_value());

      // Filter for Beatles
      CHECK(matchesFilter(*optRow1, "beatles") == true);
      CHECK(matchesFilter(*optRow2, "beatles") == false);
      CHECK(matchesFilter(*optRow3, "beatles") == true);
    }
  }
} // namespace ao::gtk::test
