// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

// Standalone test for app::uiapp::ui::TrackListAdapter without GTKMM dependency.
// Tests adapter functionality with test doubles for GTK objects.

#include <rs/library/MusicLibrary.h>
#include <rs/library/TrackBuilder.h>
#include <rs/library/TrackStore.h>
#include <rs/lmdb/Transaction.h>
#include <rs/model/TrackIdList.h>
#include <test/unit/lmdb/TestUtils.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rs::model::test
{
  // Test-specific RowData definition
  struct RowData
  {
    using TrackId = rs::TrackId;
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
    std::optional<std::uint32_t> coverArtId;
    bool missing = false;
  };

  using DictionaryId = rs::DictionaryId;

  /**
   * TestTrackRowProvider - Standalone test double for app::uiapp::ui::TrackListAdapter tests.
   * Named differently to avoid symbol collision when linking.
   */
  class TestTrackRowProvider final
  {
  public:
    explicit TestTrackRowProvider(rs::library::MusicLibrary& ml);

    std::optional<RowData> getRow(rs::TrackId id);
    void invalidateHot(rs::TrackId id);
    void invalidateFull(rs::TrackId id);
    void remove(rs::TrackId id);

  private:
    std::string resolveDictionaryString(DictionaryId id);

    rs::library::MusicLibrary* _ml;
    rs::library::TrackStore* _store;
    rs::library::DictionaryStore* _dict;

    std::unordered_map<rs::TrackId, RowData> _rowCache;
    std::unordered_map<DictionaryId, std::string> _stringCache;
  };

  std::string TestTrackRowProvider::resolveDictionaryString(DictionaryId id)
  {
    auto const it = _stringCache.find(id);

    if (it != _stringCache.end())
    {
      return it->second;
    }

    std::string result;

    try
    {
      auto const str = _dict->get(id);
      result = std::string(str);
    }
    catch (std::exception const&)
    {
      result = {};
    }

    auto const insertResult = _stringCache.emplace(id, result);

    return insertResult.first->second;
  }

  TestTrackRowProvider::TestTrackRowProvider(rs::library::MusicLibrary& ml)
    : _ml{&ml}, _store{&ml.tracks()}, _dict{&ml.dictionary()}
  {
  }

  std::optional<RowData> TestTrackRowProvider::getRow(rs::TrackId id)
  {
    auto const it = _rowCache.find(id);

    if (it != _rowCache.end())
    {
      if (it->second.missing)
      {
        return std::nullopt;
      }

      return it->second;
    }

    rs::lmdb::ReadTransaction txn(_ml->readTransaction());
    auto reader = _store->reader(txn);

    auto const optView = reader.get(id, rs::library::TrackStore::Reader::LoadMode::Both);

    if (!optView)
    {
      RowData row;
      row.id = id;
      row.missing = true;
      _rowCache.emplace(id, std::move(row));

      return std::nullopt;
    }

    auto const& view = *optView;
    auto const& metadata = view.metadata();

    RowData row;
    row.id = id;
    row.title = std::string(metadata.title());

    auto const artistId = metadata.artistId();

    if (artistId != DictionaryId{0})
    {
      row.artist = resolveDictionaryString(artistId);
    }

    auto const albumId = metadata.albumId();

    if (albumId != DictionaryId{0})
    {
      row.album = resolveDictionaryString(albumId);
    }

    auto const albumArtistId = metadata.albumArtistId();

    if (albumArtistId != DictionaryId{0})
    {
      row.albumArtist = resolveDictionaryString(albumArtistId);
    }

    auto const genreId = metadata.genreId();

    if (genreId != DictionaryId{0})
    {
      row.genre = resolveDictionaryString(genreId);
    }

    row.year = metadata.year();
    row.discNumber = metadata.discNumber();
    row.trackNumber = metadata.trackNumber();

    auto const coverArtId = metadata.coverArtId();

    if (coverArtId != 0)
    {
      row.coverArtId = coverArtId;
    }

    auto const result = _rowCache.emplace(id, std::move(row));

    return result.first->second;
  }

  void TestTrackRowProvider::invalidateHot(rs::TrackId id)
  {
    _rowCache.erase(id);
  }

  void TestTrackRowProvider::invalidateFull(rs::TrackId id)
  {
    _rowCache.erase(id);
  }

  void TestTrackRowProvider::remove(rs::TrackId id)
  {
    _rowCache.erase(id);
  }

} // namespace rs::model::test

namespace
{

  using rs::TrackId;
  using rs::library::MusicLibrary;
  using rs::library::TrackBuilder;
  using rs::library::TrackStore;
  using rs::model::TrackIdList;
  using rs::model::TrackIdListObserver;
  using rs::model::test::RowData;
  using rs::model::test::TestTrackRowProvider;

  struct TrackSpec
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
      : _tempDir{}, _library{_tempDir.path()}
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

      auto hotData = builder.serializeHot(txn, _library.dictionary());
      auto coldData = builder.serializeCold(txn, _library.dictionary(), _library.resources());
      auto [id, _view] = writer.createHotCold(hotData, coldData);
      (void)_view;
      txn.commit();

      return id;
    }

  private:
    TempDir _tempDir;
    MusicLibrary _library;
  };

  /**
   * MutableTrackIdList - Test helper for TrackIdList.
   */
  class MutableTrackIdList final : public TrackIdList
  {
  public:
    void addInitial(TrackId id) { _ids.push_back(id); }

    void insert(TrackId id, std::size_t index)
    {
      _ids.insert(_ids.begin() + static_cast<std::ptrdiff_t>(index), id);
      notifyInserted(id, index);
    }

    void update(TrackId id)
    {
      auto const index = indexOf(id);
      REQUIRE(index.has_value());
      notifyUpdated(id, *index);
    }

    void onReset() { notifyReset(); }

    void remove(TrackId id)
    {
      auto const index = indexOf(id);
      REQUIRE(index.has_value());
      _ids.erase(_ids.begin() + static_cast<std::ptrdiff_t>(*index));
      notifyRemoved(id, *index);
    }

    std::size_t size() const override { return _ids.size(); }

    TrackId trackIdAt(std::size_t index) const override { return _ids.at(index); }

    std::optional<std::size_t> indexOf(TrackId id) const override
    {
      auto const it = std::ranges::find(_ids, id);

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
  class ObserverSpy final : public TrackIdListObserver
  {
  public:
    enum class EventKind
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
   * Filter matcher - standalone version matching app::uiapp::ui::TrackListAdapter logic.
   */
  bool matchesFilter(RowData const& rowData, std::string_view filter)
  {
    if (filter.empty())
    {
      return true;
    }

    auto needle = std::string{filter};
    std::ranges::transform(needle, needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto check = [&needle](std::string const& field) -> bool
    {
      if (field.empty())
      {
        return false;
      }

      auto lower = field;
      std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

      return lower.find(needle) != std::string::npos;
    };

    return check(rowData.artist) || check(rowData.album) || check(rowData.albumArtist) || check(rowData.genre) ||
           check(rowData.title) || check(rowData.tags);
  }

} // namespace

TEST_CASE("app::uiapp::ui::TrackListAdapter", "[app][adapter]")
{
  SECTION("TestTrackRowProvider loads row data for filtering")
  {
    auto testLibrary = TestMusicLibrary{};
    auto provider = std::make_shared<TestTrackRowProvider>(testLibrary.library());
    auto const trackId = testLibrary.addTrack(makeTrackSpec("My Song", "Beatles", "Abbey Road", 2023));

    auto const row = provider->getRow(trackId);
    REQUIRE(row.has_value());
    CHECK(row->title == "My Song");
    CHECK(row->artist == "Beatles");
    CHECK(row->album == "Abbey Road");
    CHECK(row->year == 2023);
  }

  SECTION("filter matcher works for artist")
  {
    auto testLibrary = TestMusicLibrary{};
    auto provider = std::make_shared<TestTrackRowProvider>(testLibrary.library());
    auto const trackId = testLibrary.addTrack(makeTrackSpec("Song", "Beatles", "Album", 2020));

    auto const row = provider->getRow(trackId);
    REQUIRE(row.has_value());

    CHECK(matchesFilter(*row, "beatles") == true);
    CHECK(matchesFilter(*row, "stones") == false);
  }

  SECTION("filter matcher works for album")
  {
    auto testLibrary = TestMusicLibrary{};
    auto provider = std::make_shared<TestTrackRowProvider>(testLibrary.library());
    auto const trackId = testLibrary.addTrack(makeTrackSpec("Song", "Artist", "Abbey Road", 2020));

    auto const row = provider->getRow(trackId);
    REQUIRE(row.has_value());

    CHECK(matchesFilter(*row, "abbey") == true);
    CHECK(matchesFilter(*row, "revolver") == false);
  }

  SECTION("filter matcher works for title")
  {
    auto testLibrary = TestMusicLibrary{};
    auto provider = std::make_shared<TestTrackRowProvider>(testLibrary.library());
    auto const trackId = testLibrary.addTrack(makeTrackSpec("Yesterday", "Artist", "Album", 2020));

    auto const row = provider->getRow(trackId);
    REQUIRE(row.has_value());

    CHECK(matchesFilter(*row, "yesterday") == true);
    CHECK(matchesFilter(*row, "today") == false);
  }

  SECTION("filter matcher is case-insensitive")
  {
    auto testLibrary = TestMusicLibrary{};
    auto provider = std::make_shared<TestTrackRowProvider>(testLibrary.library());
    auto const trackId = testLibrary.addTrack(makeTrackSpec("Song", "The Beatles", "Album", 2020));

    auto const row = provider->getRow(trackId);
    REQUIRE(row.has_value());

    CHECK(matchesFilter(*row, "BEATLES") == true);
    CHECK(matchesFilter(*row, "beatles") == true);
    CHECK(matchesFilter(*row, "BeAtLeS") == true);
  }

  SECTION("filter matcher returns true for empty filter")
  {
    auto testLibrary = TestMusicLibrary{};
    auto provider = std::make_shared<TestTrackRowProvider>(testLibrary.library());
    auto const trackId = testLibrary.addTrack(makeTrackSpec("Song", "Artist", "Album", 2020));

    auto const row = provider->getRow(trackId);
    REQUIRE(row.has_value());

    CHECK(matchesFilter(*row, "") == true);
  }

  SECTION("MutableTrackIdList notifies observers on insert")
  {
    auto source = MutableTrackIdList{};
    auto spy = ObserverSpy{};
    source.attach(&spy);

    auto const id = TrackId{1};
    source.addInitial(id);

    CHECK(spy.events.size() == 0); // addInitial doesn't notify

    source.insert(TrackId{2}, 0);
    CHECK(spy.events.size() == 1);
    CHECK(spy.events[0].kind == ObserverSpy::EventKind::Inserted);
    CHECK(spy.events[0].id == TrackId{2});

    source.detach(&spy);
  }

  SECTION("MutableTrackIdList notifies observers on remove")
  {
    auto source = MutableTrackIdList{};
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

  SECTION("MutableTrackIdList notifies observers on update")
  {
    auto source = MutableTrackIdList{};
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

  SECTION("MutableTrackIdList notifies observers on reset")
  {
    auto source = MutableTrackIdList{};
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
    auto const first = provider->getRow(trackId);
    REQUIRE(first.has_value());

    // Invalidate
    provider->invalidateHot(trackId);

    // Should reload
    auto const second = provider->getRow(trackId);
    REQUIRE(second.has_value());
    CHECK(second->title == "Cache Test");
  }

  SECTION("multiple tracks can be loaded and filtered")
  {
    auto testLibrary = TestMusicLibrary{};
    auto provider = std::make_shared<TestTrackRowProvider>(testLibrary.library());

    auto const track1 = testLibrary.addTrack(makeTrackSpec("Song A", "Beatles", "Album 1", 2020));
    auto const track2 = testLibrary.addTrack(makeTrackSpec("Song B", "Stones", "Album 2", 2021));
    auto const track3 = testLibrary.addTrack(makeTrackSpec("Song C", "Beatles", "Album 3", 2022));

    auto const row1 = provider->getRow(track1);
    auto const row2 = provider->getRow(track2);
    auto const row3 = provider->getRow(track3);

    REQUIRE(row1.has_value());
    REQUIRE(row2.has_value());
    REQUIRE(row3.has_value());

    // Filter for Beatles
    CHECK(matchesFilter(*row1, "beatles") == true);
    CHECK(matchesFilter(*row2, "beatles") == false);
    CHECK(matchesFilter(*row3, "beatles") == true);
  }
}
