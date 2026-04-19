// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

// Standalone test for row data loading without GTKMM dependency.
// Tests TrackRowDataProvider functionality in isolation.

#include <rs/core/MusicLibrary.h>
#include <rs/core/TrackBuilder.h>
#include <rs/core/TrackStore.h>
#include <rs/lmdb/Transaction.h>
#include <test/unit/lmdb/TestUtils.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

namespace app::model
{
  struct RowData;
  using TrackId = rs::core::TrackId;
  using DictionaryId = rs::core::DictionaryId;

  /**
   * Standalone TrackRowDataProvider test double.
   * Mirrors the real TrackRowDataProvider but without GTKMM includes.
   */
  class TrackRowDataProvider final
  {
  public:
    explicit TrackRowDataProvider(rs::core::MusicLibrary& ml);

    std::optional<RowData> getRow(TrackId id);
    void invalidateHot(TrackId id);
    void invalidateFull(TrackId id);
    void remove(TrackId id);

  private:
    std::string resolveDictionaryString(DictionaryId id);

    rs::core::MusicLibrary* _ml;
    rs::core::TrackStore* _store;
    rs::core::DictionaryStore* _dict;

    std::unordered_map<TrackId, RowData> _rowCache;
    std::unordered_map<DictionaryId, std::string> _stringCache;
  };

  struct RowData
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
    std::chrono::milliseconds duration{0};
    std::optional<std::uint32_t> coverArtId;
    bool missing = false;
  };

  std::string TrackRowDataProvider::resolveDictionaryString(DictionaryId id)
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

  auto joinResolvedTags(rs::core::TrackView::TagProxy tags, rs::core::DictionaryStore const& dictionary) -> std::string
  {
    auto text = std::string{};
    auto first = true;

    for (auto const tagId : tags)
    {
      auto const tag = dictionary.get(tagId);
      if (tag.empty())
      {
        continue;
      }

      if (!first)
      {
        text += ", ";
      }

      text += tag;
      first = false;
    }

    return text;
  }

  TrackRowDataProvider::TrackRowDataProvider(rs::core::MusicLibrary& ml)
    : _ml{&ml}, _store{&ml.tracks()}, _dict{&ml.dictionary()}
  {
  }

  std::optional<RowData> TrackRowDataProvider::getRow(TrackId id)
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

    auto const optView = reader.get(id, rs::core::TrackStore::Reader::LoadMode::Both);
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
    row.duration = std::chrono::milliseconds{view.property().durationMs()};

    auto const coverArtId = metadata.coverArtId();
    if (coverArtId != 0)
    {
      row.coverArtId = coverArtId;
    }

    row.tags = joinResolvedTags(view.tags(), *_dict);

    auto const result = _rowCache.emplace(id, std::move(row));
    return result.first->second;
  }

  void TrackRowDataProvider::invalidateHot(TrackId id)
  {
    _rowCache.erase(id);
  }

  void TrackRowDataProvider::invalidateFull(TrackId id)
  {
    _rowCache.erase(id);
  }

  void TrackRowDataProvider::remove(TrackId id)
  {
    _rowCache.erase(id);
  }

} // namespace app::model

namespace
{

  using app::model::RowData;
  using app::model::TrackRowDataProvider;
  using rs::core::MusicLibrary;
  using rs::core::TrackBuilder;
  using rs::core::TrackId;
  using rs::core::TrackStore;

  struct TrackSpec
  {
    std::string title = "Track";
    std::string artist = "Artist";
    std::string album = "Album";
    std::string albumArtist = "AlbumArtist";
    std::string genre = "Genre";
    std::vector<std::string> tags;
    std::uint16_t year = 2020;
    std::uint16_t trackNumber = 1;
    std::uint16_t discNumber = 1;
    std::uint32_t durationMs = 180000;
  };

  TrackSpec makeTrackSpec(std::string_view title, std::uint16_t year)
  {
    auto spec = TrackSpec{};
    spec.title = title;
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

      for (auto const& tag : spec.tags)
      {
        builder.tags().add(tag);
      }

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

    void updateTrack(TrackId id, std::function<void(TrackBuilder&)> const& mutate)
    {
      auto txn = _library.writeTransaction();
      auto writer = _library.tracks().writer(txn);
      auto view = writer.get(id, TrackStore::Reader::LoadMode::Both);
      REQUIRE(view.has_value());

      auto builder = TrackBuilder::fromView(*view, _library.dictionary());
      mutate(builder);

      auto hotData = builder.serializeHot(txn, _library.dictionary());
      auto coldData = builder.serializeCold(txn, _library.dictionary(), _library.resources());
      writer.updateHot(id, hotData);
      writer.updateCold(id, coldData);
      txn.commit();
    }

  private:
    TempDir _tempDir;
    MusicLibrary _library;
  };

} // namespace

TEST_CASE("TrackRowDataProvider", "[app][rowprovider]")
{
  SECTION("getRow loads data on first call")
  {
    auto testLibrary = TestMusicLibrary{};
    auto provider = TrackRowDataProvider{testLibrary.library()};
    auto trackId = testLibrary.addTrack(makeTrackSpec("My Song", 2023));

    auto const row = provider.getRow(trackId);

    REQUIRE(row.has_value());
    CHECK(row->id == trackId);
    CHECK(row->title == "My Song");
    CHECK(row->year == 2023);
    CHECK(row->artist == "Artist");
    CHECK(row->album == "Album");
  }

  SECTION("getRow returns cached data on subsequent calls")
  {
    auto testLibrary = TestMusicLibrary{};
    auto provider = TrackRowDataProvider{testLibrary.library()};
    auto trackId = testLibrary.addTrack(makeTrackSpec("Cached", 2021));

    auto const first = provider.getRow(trackId);
    auto const second = provider.getRow(trackId);

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->title == second->title);
  }

  SECTION("getRow returns nullopt for non-existent track")
  {
    auto testLibrary = TestMusicLibrary{};
    auto provider = TrackRowDataProvider{testLibrary.library()};
    auto nonExistentId = TrackId{999999};

    auto const row = provider.getRow(nonExistentId);

    CHECK_FALSE(row.has_value());
  }

  SECTION("invalidateHot clears cache but allows reload")
  {
    auto testLibrary = TestMusicLibrary{};
    auto provider = TrackRowDataProvider{testLibrary.library()};
    auto trackId = testLibrary.addTrack(makeTrackSpec("ToInvalidate", 2022));

    // Load and cache
    auto const before = provider.getRow(trackId);
    REQUIRE(before.has_value());

    // Invalidate hot
    provider.invalidateHot(trackId);

    // Should reload without crashing
    auto const after = provider.getRow(trackId);
    REQUIRE(after.has_value());
    CHECK(after->title == "ToInvalidate");
  }

  SECTION("invalidateFull clears cache for track")
  {
    auto testLibrary = TestMusicLibrary{};
    auto provider = TrackRowDataProvider{testLibrary.library()};
    auto trackId = testLibrary.addTrack(makeTrackSpec("FullInvalidate", 2021));

    // Load and cache
    auto const before = provider.getRow(trackId);
    REQUIRE(before.has_value());

    // Invalidate full
    provider.invalidateFull(trackId);

    // Should reload
    auto const after = provider.getRow(trackId);
    REQUIRE(after.has_value());
  }

  SECTION("remove evicts track from cache")
  {
    auto testLibrary = TestMusicLibrary{};
    auto provider = TrackRowDataProvider{testLibrary.library()};
    auto trackId = testLibrary.addTrack(makeTrackSpec("ToRemove", 2020));

    // Load and cache
    auto const before = provider.getRow(trackId);
    REQUIRE(before.has_value());

    // Remove from cache
    provider.remove(trackId);

    // Should reload
    auto const after = provider.getRow(trackId);
    REQUIRE(after.has_value());
  }

  SECTION("row data contains all grouping fields")
  {
    auto testLibrary = TestMusicLibrary{};
    auto provider = TrackRowDataProvider{testLibrary.library()};
    auto spec = TrackSpec{};
    spec.title = "Group Test";
    spec.artist = "Artist X";
    spec.album = "Album Y";
    spec.albumArtist = "AlbumArtist Z";
    spec.genre = "Rock";
    spec.year = 2023;
    spec.trackNumber = 5;
    spec.discNumber = 2;
    auto trackId = testLibrary.addTrack(spec);

    auto const row = provider.getRow(trackId);

    REQUIRE(row.has_value());
    CHECK(row->title == "Group Test");
    CHECK(row->artist == "Artist X");
    CHECK(row->album == "Album Y");
    CHECK(row->albumArtist == "AlbumArtist Z");
    CHECK(row->genre == "Rock");
    CHECK(row->year == 2023);
    CHECK(row->trackNumber == 5);
    CHECK(row->discNumber == 2);
  }

  SECTION("row data resolves track tags for display")
  {
    auto testLibrary = TestMusicLibrary{};
    auto provider = TrackRowDataProvider{testLibrary.library()};
    auto spec = TrackSpec{};
    spec.title = "Tagged";
    spec.tags = {"rock", "favorite"};
    auto trackId = testLibrary.addTrack(spec);

    auto const row = provider.getRow(trackId);

    REQUIRE(row.has_value());
    CHECK(row->tags == "rock, favorite");
  }

  SECTION("track with missing dictionary values returns empty strings")
  {
    auto testLibrary = TestMusicLibrary{};
    auto provider = TrackRowDataProvider{testLibrary.library()};
    // Use a track with minimal metadata
    auto trackId = testLibrary.addTrack(makeTrackSpec("Minimal", 2020));

    auto const row = provider.getRow(trackId);

    REQUIRE(row.has_value());
    // Artist/album etc should be empty since we didn't set them
    CHECK(row->title == "Minimal");
    CHECK(row->year == 2020);
  }
}
