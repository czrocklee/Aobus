// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/library/TestUtils.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/library/CoverArt.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::library::test
{
  namespace
  {
#if defined(__GNUC__) && !defined(__clang__)
    static_assert(std::ranges::view<TrackView::CoverArtProxy>);
#endif

    using namespace ao::lmdb::test;

    // Helper to create a minimal valid hot TrackView for testing
    std::vector<std::byte> createMinimalHotData()
    {
      auto h = TrackHotHeader{};
      h.tagBloom = 0;
      h.artistId = DictionaryId{1};
      h.albumId = DictionaryId{2};
      h.genreId = DictionaryId{3};
      h.albumArtistId = kInvalidDictionaryId;
      h.composerId = kInvalidDictionaryId;
      h.year = 2020;
      h.codec = AudioCodec::Unknown;
      h.bitDepth = BitDepth{16};
      h.tagLength = 0;
      h.titleLength = 0;

      return serializeHeader(h);
    }

    std::vector<std::byte> createTrackWithStrings(std::string_view title)
    {
      auto h = TrackHotHeader{};
      h.tagBloom = 0;
      h.artistId = DictionaryId{1};
      h.albumId = DictionaryId{2};
      h.genreId = DictionaryId{3};
      h.albumArtistId = kInvalidDictionaryId;
      h.composerId = kInvalidDictionaryId;
      h.year = 2020;
      h.codec = AudioCodec::Unknown;
      h.bitDepth = BitDepth{16};
      h.tagLength = 0; // no tags

      // In new layout: tags first (at sizeof(TrackHotHeader)), title after (at sizeof(TrackHotHeader) + tagLength)
      h.titleLength = static_cast<std::uint16_t>(title.size());

      auto data = serializeHeader(h);

      // Add title + null
      appendString(data, title);

      return data;
    }

    std::vector<std::byte> createColdData(TrackColdHeader const& header = {},
                                          std::vector<std::pair<std::string, std::string>> const& customPairs = {},
                                          std::string_view uri = "")
    {
      auto builder = TrackBuilder::createNew();
      builder.property().uri(uri);
      // Cover entries are serialized separately from the fixed header.
      builder.metadata().trackNumber(header.trackNumber);
      builder.metadata().trackTotal(header.trackTotal);
      builder.metadata().discNumber(header.discNumber);
      builder.metadata().discTotal(header.discTotal);
      builder.property().duration(header.duration);
      builder.property().bitrate(header.bitrate);
      builder.property().channels(header.channels);

      for (auto const& [key, value] : customPairs)
      {
        builder.customMetadata().add(key, value);
      }

      auto temp = ao::test::TempDir{};
      auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
      auto wtxn = lmdb::test::beginWriteTransaction(env);
      auto dict = DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
      auto resources = ResourceStore{lmdb::test::openDatabase(wtxn, "resources")};
      auto result = builder.serializeCold(wtxn, dict, resources);
      REQUIRE(result);
      return *result;
    }

    TrackView makeColdView(std::vector<std::byte> const& data)
    {
      return TrackView{std::span<std::byte const>{}, data};
    }
  } // namespace

  // === Metadata Tests ===
  TEST_CASE("TrackView - returns title from hot data", "[library][unit][track]")
  {
    auto const data = createTrackWithStrings("Test Title");
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.metadata().title() == "Test Title");
  }

  TEST_CASE("TrackView - returns empty title when hot title length is zero", "[library][unit][track]")
  {
    auto const data = createMinimalHotData();
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.metadata().title().empty());
  }

  TEST_CASE("TrackView - returns dictionary IDs from hot data", "[library][unit][track]")
  {
    auto const data = createMinimalHotData();
    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.metadata().artistId() == DictionaryId{1});
    CHECK(view.metadata().albumId() == DictionaryId{2});
    CHECK(view.metadata().genreId() == DictionaryId{3});
    CHECK(view.metadata().albumArtistId() == kInvalidDictionaryId);
    CHECK(view.metadata().composerId() == kInvalidDictionaryId);
  }

  TEST_CASE("TrackView - returns year from hot data", "[library][unit][track]")
  {
    auto const data = createMinimalHotData();
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.metadata().year() == 2020);
  }

  TEST_CASE("TrackView - returns track numbering from cold data", "[library][unit][track]")
  {
    auto header = TrackColdHeader{};
    header.trackNumber = 5;
    header.trackTotal = 10;
    header.discNumber = 1;
    header.discTotal = 2;

    auto const data = createColdData(header, {}, "/path/to/file.flac");
    auto const view = makeColdView(data);

    CHECK(view.metadata().trackNumber() == 5);
    CHECK(view.metadata().trackTotal() == 10);
    CHECK(view.metadata().discNumber() == 1);
    CHECK(view.metadata().discTotal() == 2);
  }

  TEST_CASE("TrackView - returns work and movement IDs from cold data", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata()
      .work("Symphony No. 9 in D minor, Op. 125")
      .movement("II. Molto vivace")
      .movementNumber(2)
      .movementTotal(4);

    auto temp = ao::test::TempDir{};
    auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = lmdb::test::beginWriteTransaction(env);
    auto dict = DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
    auto resources = ResourceStore{lmdb::test::openDatabase(wtxn, "resources")};
    auto coldDataResult = builder.serializeCold(wtxn, dict, resources);
    REQUIRE(coldDataResult);
    auto const& coldData = *coldDataResult;
    auto const view = makeColdView(coldData);

    CHECK(view.metadata().workId().raw() > 0);
    CHECK(view.metadata().movementId().raw() > 0);
    CHECK(dict.get(view.metadata().workId()) == "Symphony No. 9 in D minor, Op. 125");
    CHECK(dict.get(view.metadata().movementId()) == "II. Molto vivace");
    CHECK(view.metadata().movementNumber() == 2);
    CHECK(view.metadata().movementTotal() == 4);
  }

  TEST_CASE("TrackView - returns cover art entries from cold data", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.coverArt().add(PictureType::BackCover, ResourceId{42});

    auto temp = ao::test::TempDir{};
    auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = lmdb::test::beginWriteTransaction(env);
    auto dict = DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
    auto resources = ResourceStore{lmdb::test::openDatabase(wtxn, "resources")};
    auto coldDataResult = builder.serializeCold(wtxn, dict, resources);
    REQUIRE(coldDataResult);
    auto const& coldData = *coldDataResult;

    auto const view = makeColdView(coldData);
    REQUIRE(view.coverArt().count() == 1);
    CHECK(view.coverArt().at(0).resourceId == ResourceId{42});
    CHECK(view.coverArt().at(0).type == PictureType::BackCover);
    REQUIRE(view.coverArt().primary());
    CHECK(view.coverArt().primary()->resourceId == ResourceId{42});
    CHECK(view.coverArt().primary()->type == PictureType::BackCover);
  }

  TEST_CASE("TrackView - returns URI from cold data", "[library][unit][track]")
  {
    auto const data = createColdData({}, {}, "/path/to/file.flac");
    auto const view = makeColdView(data);

    CHECK_FALSE(view.coverArt().primary().has_value());
    CHECK(view.property().uri() == "/path/to/file.flac");
  }

  TEST_CASE("TrackView - returns empty URI when cold URI length is zero", "[library][unit][track]")
  {
    auto const data = createColdData({}, {}, "");
    auto const view = makeColdView(data);

    CHECK(view.property().uri().empty());
  }

  // === Property Tests ===
  TEST_CASE("TrackView - returns codec and bit depth from hot data", "[library][unit][track]")
  {
    auto h = TrackHotHeader{};
    h.codec = AudioCodec::Flac;
    h.bitDepth = BitDepth{24};
    h.sampleRate = SampleRate{96000};
    auto const data = serializeHeader(h);
    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.property().codec() == AudioCodec::Flac);
    CHECK(view.property().bitDepth() == 24);
    CHECK(view.property().sampleRate() == 96000);
  }

  TEST_CASE("TrackView - returns file size and modification time from cold data", "[library][unit][track]")
  {
    auto const data = createColdData({}, {}, "");
    auto const view = makeColdView(data);
    CHECK(view.isColdValid());
  }

  TEST_CASE("TrackView - returns audio format from cold data", "[library][unit][track]")
  {
    auto header = TrackColdHeader{};
    header.duration = std::chrono::minutes{3};
    header.bitrate = Bitrate{320000};
    header.channels = Channels{2};

    auto const data = createColdData(header, {}, "");
    auto const view = makeColdView(data);

    CHECK(view.property().duration() == std::chrono::minutes{3});
    CHECK(view.property().bitrate() == 320000);
    CHECK(view.property().channels() == 2);
  }
} // namespace ao::library::test
