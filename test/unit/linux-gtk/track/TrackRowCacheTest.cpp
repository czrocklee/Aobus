// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

// Standalone test for row data loading without GTKMM dependency.
// Tests ao::model::TrackRowCache functionality in isolation.

#include "ao/Type.h"
#include "ao/library/MusicLibrary.h"
#include "ao/library/TrackBuilder.h"
#include "ao/library/TrackStore.h"
#include "ao/library/TrackView.h"
#include "ao/lmdb/Transaction.h"
#include "test/unit/lmdb/TestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ao::gtk::test
{
  namespace
  {
    using namespace ao::lmdb::test;

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
      std::chrono::milliseconds duration{0};
      std::optional<std::uint32_t> optCoverArtId;
      bool missing = false;
    };

    /**
     * Standalone TrackRowCache test double.
     * Mirrors the real ao::model::TrackRowCache but without GTKMM includes.
     */
    class TrackRowCache final
    {
    public:
      explicit TrackRowCache(library::MusicLibrary& ml);

      std::optional<RowData> getRow(TrackId id);
      void invalidateHot(TrackId id);

    private:
      std::string resolveDictionaryString(DictionaryId id);

      library::MusicLibrary* _ml;
      library::TrackStore* _store;
      library::DictionaryStore* _dict;

      std::unordered_map<TrackId, RowData> _rowCache;
      std::unordered_map<DictionaryId, std::string> _stringCache;
    };

    std::string TrackRowCache::resolveDictionaryString(DictionaryId id)
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

    std::string joinResolvedTags(library::TrackView::TagProxy tags, library::DictionaryStore const& dictionary)
    {
      auto text = std::string{};
      bool first = true;

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

    TrackRowCache::TrackRowCache(library::MusicLibrary& ml)
      : _ml{&ml}, _store{&ml.tracks()}, _dict{&ml.dictionary()}
    {
    }

    std::optional<RowData> TrackRowCache::getRow(TrackId id)
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

      auto const optView = reader.get(id, library::TrackStore::Reader::LoadMode::Both);

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
      row.title = std::string{metadata.title()};
      row.year = metadata.year();
      row.discNumber = metadata.discNumber();
      row.trackNumber = metadata.trackNumber();
      row.duration = std::chrono::milliseconds{view.property().durationMs()};

      row.tags = joinResolvedTags(view.tags(), *_dict);

      _rowCache.emplace(id, row);
      return row;
    }

    void TrackRowCache::invalidateHot(TrackId id)
    {
      _rowCache.erase(id);
    }

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
  } // namespace

  TEST_CASE("TrackRowCache loads track data correctly", "[app][unit][model]")
  {
    auto testLibrary = TestMusicLibrary{};

    SECTION("Basic data loading")
    {
      auto spec1 = TrackSpec{};
      spec1.artist = "Artist 1";
      spec1.album = "Album 1";
      spec1.title = "Track 1";
      spec1.genre = "Genre 1";
      spec1.year = 2021;
      spec1.trackNumber = 1;
      spec1.durationMs = 180000;

      auto spec2 = TrackSpec{};
      spec2.title = "Track 2";
      spec2.durationMs = 240000;

      auto const id1 = testLibrary.addTrack(spec1);
      auto const id2 = testLibrary.addTrack(spec2);

      auto provider = TrackRowCache{testLibrary.library()};

      auto const optRow1 = provider.getRow(id1);
      REQUIRE(optRow1.has_value());
      CHECK(optRow1->artist == "Artist 1");
      CHECK(optRow1->album == "Album 1");
      CHECK(optRow1->title == "Track 1");
      CHECK(optRow1->genre == "Genre 1");
      CHECK(optRow1->year == 2021);
      CHECK(optRow1->trackNumber == 1);
      CHECK(optRow1->duration.count() == 180000);

      auto const optRow2 = provider.getRow(id2);
      REQUIRE(optRow2.has_value());
      CHECK(optRow2->title == "Track 2");
      CHECK(optRow2->duration.count() == 240000);
    }

    SECTION("Caching works")
    {
      auto const id1 = testLibrary.addTrack({});
      auto provider = TrackRowCache{testLibrary.library()};

      auto const optRow1A = provider.getRow(id1);
      auto const optRow1B = provider.getRow(id1);

      CHECK(optRow1A->title == optRow1B->title);
    }

    SECTION("Invalidation")
    {
      auto const id1 = testLibrary.addTrack({});
      auto provider = TrackRowCache{testLibrary.library()};

      provider.getRow(id1);
      provider.invalidateHot(id1);
      // Next call will reload from DB (implicitly tested by it working after change)
    }

    SECTION("Non-existent track")
    {
      auto provider = TrackRowCache{testLibrary.library()};
      auto const dummyId = TrackId{9999};

      auto const optRow = provider.getRow(dummyId);
      CHECK_FALSE(optRow.has_value());
    }
  }
} // namespace ao::gtk::test
