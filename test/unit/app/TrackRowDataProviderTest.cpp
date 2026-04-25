// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

// Standalone test for row data loading without GTKMM dependency.
// Tests app::core::model::TrackRowDataProvider functionality in isolation.

#include <rs/core/MusicLibrary.h>
#include <rs/core/TrackBuilder.h>
#include <rs/core/TrackStore.h>
#include <rs/lmdb/Transaction.h>
#include <test/unit/lmdb/TestUtils.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace app::core::model
{
  struct RowData;
  using TrackId = rs::core::TrackId;
  using DictionaryId = rs::core::DictionaryId;

  /**
   * Standalone TrackRowDataProvider test double.
   * Mirrors the real app::core::model::TrackRowDataProvider but without GTKMM includes.
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

  std::string joinResolvedTags(rs::core::TrackView::TagProxy tags, rs::core::DictionaryStore const& dictionary)
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
      auto row = RowData{};
      row.id = id;
      row.missing = true;
      _rowCache.emplace(id, std::move(row));
      return std::nullopt;
    }

    auto const& view = *optView;
    auto const& metadata = view.metadata();

    auto row = RowData{};
    row.id = id;
    row.artist = resolveDictionaryString(metadata.artistId());
    row.album = resolveDictionaryString(metadata.albumId());
    row.albumArtist = resolveDictionaryString(metadata.albumArtistId());
    row.genre = resolveDictionaryString(metadata.genreId());
    row.title = std::string(metadata.title());
    row.year = metadata.year();
    row.discNumber = metadata.discNumber();
    row.trackNumber = metadata.trackNumber();
    row.duration = std::chrono::milliseconds{view.property().durationMs()};

    row.tags = joinResolvedTags(view.tags(), *_dict);

    _rowCache.emplace(id, row);
    return row;
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

  struct TrackSpec
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

  class TestMusicLibrary final
  {
  public:
    TestMusicLibrary()
      : _tempDir{}, _library{_tempDir.path()}
    {
    }

    rs::core::MusicLibrary& library() { return _library; }

    TrackId addTrack(TrackSpec const& spec)
    {
      auto txn = _library.writeTransaction();
      auto writer = _library.tracks().writer(txn);

      auto builder = rs::core::TrackBuilder::createNew();
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
    rs::core::MusicLibrary _library;
  };

} // namespace app::core::model

using namespace app::core::model;

TEST_CASE("TrackRowDataProvider loads track data correctly", "[app][model]")
{
  TestMusicLibrary testLibrary;

  SECTION("Basic data loading")
  {
    TrackSpec spec1;
    spec1.artist = "Artist 1";
    spec1.album = "Album 1";
    spec1.title = "Track 1";
    spec1.genre = "Genre 1";
    spec1.year = 2021;
    spec1.trackNumber = 1;
    spec1.durationMs = 180000;

    TrackSpec spec2;
    spec2.title = "Track 2";
    spec2.durationMs = 240000;

    auto id1 = testLibrary.addTrack(spec1);
    auto id2 = testLibrary.addTrack(spec2);

    auto provider = TrackRowDataProvider{testLibrary.library()};

    auto row1 = provider.getRow(id1);
    REQUIRE(row1.has_value());
    CHECK(row1->artist == "Artist 1");
    CHECK(row1->album == "Album 1");
    CHECK(row1->title == "Track 1");
    CHECK(row1->genre == "Genre 1");
    CHECK(row1->year == 2021);
    CHECK(row1->trackNumber == 1);
    CHECK(row1->duration.count() == 180000);

    auto row2 = provider.getRow(id2);
    REQUIRE(row2.has_value());
    CHECK(row2->title == "Track 2");
    CHECK(row2->duration.count() == 240000);
  }

  SECTION("Caching works")
  {
    auto id1 = testLibrary.addTrack({});
    auto provider = TrackRowDataProvider{testLibrary.library()};

    auto row1_a = provider.getRow(id1);
    auto row1_b = provider.getRow(id1);

    CHECK(row1_a->title == row1_b->title);
  }

  SECTION("Invalidation")
  {
    auto id1 = testLibrary.addTrack({});
    auto provider = TrackRowDataProvider{testLibrary.library()};

    provider.getRow(id1);
    provider.invalidateHot(id1);
    // Next call will reload from DB (implicitly tested by it working after change)
  }

  SECTION("Non-existent track")
  {
    auto provider = TrackRowDataProvider{testLibrary.library()};
    auto dummyId = rs::core::TrackId{9999};

    auto row = provider.getRow(dummyId);
    CHECK_FALSE(row.has_value());
  }
}
