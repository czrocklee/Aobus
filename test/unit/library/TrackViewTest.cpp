// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/library/TestUtils.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/Type.h>
#include <ao/library/AudioCodec.h>
#include <ao/library/CoverArt.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::library::test
{
  namespace
  {
#if defined(__GNUC__) && !defined(__clang__)
    static_assert(std::ranges::view<TrackView::TagProxy>);
    static_assert(std::ranges::view<TrackView::CustomMetadataProxy>);
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

      auto temp = TempDir{};
      auto env = lmdb::Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
      auto wtxn = lmdb::WriteTransaction{env};
      auto dict = DictionaryStore{lmdb::Database{wtxn, "dict"}, wtxn};
      auto resources = ResourceStore{lmdb::Database{wtxn, "resources"}};
      return builder.serializeCold(wtxn, dict, resources);
    }

    TrackView makeColdView(std::vector<std::byte> const& data)
    {
      return TrackView{std::span<std::byte const>{}, data};
    }
  } // namespace

  // === Metadata Tests ===
  TEST_CASE("TrackView - Hot Title", "[library][unit][track]")
  {
    auto const data = createTrackWithStrings("Test Title");
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.metadata().title() == "Test Title");
  }

  TEST_CASE("TrackView - Hot Title Empty", "[library][unit][track]")
  {
    auto const data = createMinimalHotData();
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.metadata().title().empty());
  }

  TEST_CASE("TrackView - Hot Dictionary IDs", "[library][unit][track]")
  {
    auto const data = createMinimalHotData();
    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.metadata().artistId() == DictionaryId{1});
    CHECK(view.metadata().albumId() == DictionaryId{2});
    CHECK(view.metadata().genreId() == DictionaryId{3});
    CHECK(view.metadata().albumArtistId() == kInvalidDictionaryId);
    CHECK(view.metadata().composerId() == kInvalidDictionaryId);
  }

  TEST_CASE("TrackView - Hot Year", "[library][unit][track]")
  {
    auto const data = createMinimalHotData();
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.metadata().year() == 2020);
  }

  TEST_CASE("TrackView - Cold Track Info", "[library][unit][track]")
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

  TEST_CASE("TrackView - Cold Work and Movement", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.metadata()
      .work("Symphony No. 9 in D minor, Op. 125")
      .movement("II. Molto vivace")
      .movementNumber(2)
      .movementTotal(4);

    auto temp = TempDir{};
    auto env = lmdb::Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = lmdb::WriteTransaction{env};
    auto dict = DictionaryStore{lmdb::Database{wtxn, "dict"}, wtxn};
    auto resources = ResourceStore{lmdb::Database{wtxn, "resources"}};
    auto const coldData = builder.serializeCold(wtxn, dict, resources);
    auto const view = makeColdView(coldData);

    CHECK(view.metadata().workId().raw() > 0);
    CHECK(view.metadata().movementId().raw() > 0);
    CHECK(dict.get(view.metadata().workId()) == "Symphony No. 9 in D minor, Op. 125");
    CHECK(dict.get(view.metadata().movementId()) == "II. Molto vivace");
    CHECK(view.metadata().movementNumber() == 2);
    CHECK(view.metadata().movementTotal() == 4);
  }

  TEST_CASE("TrackView - Cold Cover Art", "[library][unit][track]")
  {
    auto builder = TrackBuilder::createNew();
    builder.coverArt().add(PictureType::BackCover, ResourceId{42});

    auto temp = TempDir{};
    auto env = lmdb::Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = lmdb::WriteTransaction{env};
    auto dict = DictionaryStore{lmdb::Database{wtxn, "dict"}, wtxn};
    auto resources = ResourceStore{lmdb::Database{wtxn, "resources"}};
    auto const coldData = builder.serializeCold(wtxn, dict, resources);

    auto const view = makeColdView(coldData);
    REQUIRE(view.coverArt().count() == 1);
    CHECK(view.coverArt().at(0).resourceId == ResourceId{42});
    CHECK(view.coverArt().at(0).type == PictureType::BackCover);
    REQUIRE(view.coverArt().primary());
    CHECK(view.coverArt().primary()->resourceId == ResourceId{42});
    CHECK(view.coverArt().primary()->type == PictureType::BackCover);
  }

  TEST_CASE("TrackView - Cold Uri", "[library][unit][track]")
  {
    auto const data = createColdData({}, {}, "/path/to/file.flac");
    auto const view = makeColdView(data);

    CHECK_FALSE(view.coverArt().primary().has_value());
    CHECK(view.property().uri() == "/path/to/file.flac");
  }

  TEST_CASE("TrackView - Cold Uri Empty", "[library][unit][track]")
  {
    auto const data = createColdData({}, {}, "");
    auto const view = makeColdView(data);

    CHECK(view.property().uri().empty());
  }

  // === Property Tests ===
  TEST_CASE("TrackView - Hot Codec and BitDepth", "[library][unit][track]")
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

  TEST_CASE("TrackView - Cold File Size and Mtime", "[library][unit][track]")
  {
    auto const data = createColdData({}, {}, "");
    auto const view = makeColdView(data);
    CHECK(view.isColdValid());
  }

  TEST_CASE("TrackView - Cold Audio Format", "[library][unit][track]")
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

  // === Tag Tests ===
  TEST_CASE("TrackView - Bloom Filter", "[library][unit][track]")
  {
    auto h = TrackHotHeader{};
    h.tagBloom = 0xCAFE;

    auto const data = serializeHeader(h);
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.tags().bloom() == 0xCAFE);
  }

  TEST_CASE("TrackView - Tag Count Zero", "[library][unit][track]")
  {
    auto const data = createTrackWithStrings("Test");
    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.tags().count() == 0);
  }

  TEST_CASE("TrackView - Tag Iterator Empty", "[library][unit][track]")
  {
    auto const data = createTrackWithStrings("Test");
    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.tags().begin() == view.tags().end());
  }

  TEST_CASE("TrackView - Tag Count With Tags", "[library][unit][track]")
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
    h.tagLength = 8; // 2 tags * 4 bytes

    auto const title = std::string{"Test Title"};
    h.titleLength = static_cast<std::uint16_t>(title.size());

    auto data = serializeHeader(h);

    std::uint32_t const tag1 = 10;
    std::uint32_t const tag2 = 20;
    data.insert_range(data.end(), utility::bytes::view(tag1));
    data.insert_range(data.end(), utility::bytes::view(tag2));

    appendString(data, title);

    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.tags().count() == 2);
  }

  TEST_CASE("TrackView - Tag Iterator", "[library][unit][track]")
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
    h.tagLength = 8; // 2 tags * 4 bytes

    auto const title = std::string{"Test Title"};
    h.titleLength = static_cast<std::uint16_t>(title.size());

    auto data = serializeHeader(h);

    std::uint32_t const tag1 = 10;
    std::uint32_t const tag2 = 20;
    data.insert_range(data.end(), utility::bytes::view(tag1));
    data.insert_range(data.end(), utility::bytes::view(tag2));

    appendString(data, title);

    auto const view = TrackView{data, std::span<std::byte const>{}};

    auto ids = std::vector(view.tags().begin(), view.tags().end());
    CHECK(ids.size() == 2);
    CHECK(ids[0] == DictionaryId{10});
    CHECK(ids[1] == DictionaryId{20});
  }

  TEST_CASE("TrackView - Tag Has", "[library][unit][track]")
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
    h.tagLength = 8;

    auto data = serializeHeader(h);
    std::uint32_t const tag1 = 10;
    std::uint32_t const tag2 = 20;
    data.insert_range(data.end(), utility::bytes::view(tag1));
    data.insert_range(data.end(), utility::bytes::view(tag2));

    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.tags().has(DictionaryId{10}) == true);
    CHECK(view.tags().has(DictionaryId{20}) == true);
    CHECK(view.tags().has(DictionaryId{30}) == false);
  }

  TEST_CASE("TrackView - Tag Id Accessor", "[library][unit][track]")
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
    h.tagLength = 8;

    auto data = serializeHeader(h);
    std::uint32_t const tag1 = 10;
    std::uint32_t const tag2 = 20;
    data.insert_range(data.end(), utility::bytes::view(tag1));
    data.insert_range(data.end(), utility::bytes::view(tag2));

    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.tags().id(0) == DictionaryId{10});
    CHECK(view.tags().id(1) == DictionaryId{20});
  }

  // === Custom Tests ===
  TEST_CASE("TrackView - Custom Roundtrip Empty", "[library][unit][track]")
  {
    auto const data = createColdData();
    auto const view = makeColdView(data);

    std::int32_t count = 0;

    for ([[maybe_unused]] auto const& [k, v] : view.customMetadata())
    {
      ++count;
    }

    CHECK(count == 0);
  }

  TEST_CASE("TrackView - Custom Roundtrip Single Pair", "[library][unit][track]")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"key1", "value1"}};
    auto const data = createColdData({}, pairs, "/path/to/file.flac");
    auto const view = makeColdView(data);

    std::int32_t count = 0;

    for (auto const& [k, v] : view.customMetadata())
    {
      CHECK(k == DictionaryId{1});
      CHECK(v == "value1");
      ++count;
    }

    CHECK(count == 1);
    CHECK(view.property().uri() == "/path/to/file.flac");
  }

  TEST_CASE("TrackView - Custom Roundtrip Multiple Pairs", "[library][unit][track]")
  {
    auto const key1 = std::string{"replaygain_track_gain_db"};
    auto const key2 = std::string{"isrc"};
    auto const key3 = std::string{"edition"};
    auto const pairs = std::vector{std::pair{key1, std::string{"-6.5"}},
                                   std::pair{key2, std::string{"USSM19999999"}},
                                   std::pair{key3, std::string{"remaster"}}};

    auto const data = createColdData({}, pairs, "/path/to/file.flac");
    auto const view = makeColdView(data);

    // DictionaryStore assigns sequential IDs: key1->1, key2->2, key3->3
    auto const id0 = DictionaryId{1};
    auto const id1 = DictionaryId{2};
    auto const id2 = DictionaryId{3};

    auto result = std::vector<std::pair<DictionaryId, std::string_view>>{};

    for (auto const& [k, v] : view.customMetadata())
    {
      result.emplace_back(k, v);
    }

    CHECK(result.size() == 3);
    CHECK(result[0].first == id0);
    CHECK(result[0].second == "-6.5");
    CHECK(result[1].first == id1);
    CHECK(result[1].second == "USSM19999999");
    CHECK(result[2].first == id2);
    CHECK(result[2].second == "remaster");
  }

  TEST_CASE("TrackView - Custom Iterator Empty", "[library][unit][track]")
  {
    auto const data = createColdData({}, {}, "");
    auto const view = makeColdView(data);

    std::int32_t count = 0;

    for ([[maybe_unused]] auto const& [key, value] : view.customMetadata())
    {
      ++count;
    }

    CHECK(count == 0);
  }

  TEST_CASE("TrackView - Custom Iterator Single Pair", "[library][unit][track]")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"key1", "value1"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    std::int32_t count = 0;

    for (auto const& [key, value] : view.customMetadata())
    {
      CHECK(key == DictionaryId{1});
      CHECK(value == "value1");
      ++count;
    }

    CHECK(count == 1);
  }

  TEST_CASE("TrackView - Custom Iterator Special Characters", "[library][unit][track]")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"comment", "Hello, World! 你好"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    for (auto const& [key, value] : view.customMetadata())
    {
      CHECK(key == DictionaryId{1});
      CHECK(value == "Hello, World! 你好");
    }
  }

  TEST_CASE("TrackView - Custom Iterator Multiple Pairs", "[library][unit][track]")
  {
    auto const key1 = std::string{"replaygain_track_gain_db"};
    auto const key2 = std::string{"isrc"};
    auto const key3 = std::string{"edition"};
    auto const pairs = std::vector{std::pair{key1, std::string{"-6.5"}},
                                   std::pair{key2, std::string{"USSM19999999"}},
                                   std::pair{key3, std::string{"remaster"}}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    auto result = std::vector<std::pair<DictionaryId, std::string_view>>{};

    for (auto const& [key, value] : view.customMetadata())
    {
      result.emplace_back(key, value);
    }

    CHECK(result.size() == 3);
    CHECK(result[0].first == DictionaryId{1});
    CHECK(result[0].second == "-6.5");
    CHECK(result[1].first == DictionaryId{2});
    CHECK(result[1].second == "USSM19999999");
    CHECK(result[2].first == DictionaryId{3});
    CHECK(result[2].second == "remaster");
  }

  TEST_CASE("TrackView - Custom Get Found", "[library][unit][track]")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"replaygain_track_gain_db", "-6.5"},
                                   std::pair<std::string, std::string>{"isrc", "USSM19999999"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    // DictionaryStore assigns: replaygain->1, isrc->2
    auto const optValue = view.customMetadata().get(DictionaryId{2});
    CHECK(optValue.has_value() == true);
    CHECK(*optValue == "USSM19999999");
  }

  TEST_CASE("TrackView - Custom Get Not Found", "[library][unit][track]")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"replaygain_track_gain_db", "-6.5"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    // ID 99 was never assigned
    auto const optValue = view.customMetadata().get(DictionaryId{99});
    CHECK(optValue.has_value() == false);
  }

  TEST_CASE("TrackView - Custom Get Case Sensitive", "[library][unit][track]")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"ISRC", "USSM19999999"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    // "ISRC" is stored at ID 1, looking up by ID 1 returns the value
    auto const optValue = view.customMetadata().get(DictionaryId{1});
    CHECK(optValue.has_value() == true);
    CHECK(*optValue == "USSM19999999");
  }

  TEST_CASE("TrackView - Custom Get Binary Search Path (64+ entries)", "[library][unit][track]")
  {
    // Create 100 entries to force binary search path
    auto pairs = std::vector<std::pair<std::string, std::string>>{};

    for (std::int32_t i = 0; i < 100; ++i)
    {
      pairs.emplace_back(std::format("key{}", i), std::format("value{}", i));
    }

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    // First entry (dictId=1)
    CHECK(view.customMetadata().get(DictionaryId{1}).value() == "value0");
    // Middle entry (dictId=50)
    CHECK(view.customMetadata().get(DictionaryId{50}).value() == "value49");
    // Last entry (dictId=100)
    CHECK(view.customMetadata().get(DictionaryId{100}).value() == "value99");
    // Not found
    CHECK(view.customMetadata().get(DictionaryId{199}).has_value() == false);
    // Before first (0 = null, should throw)
    CHECK(view.customMetadata().get(kInvalidDictionaryId).has_value() == false);
  }

  TEST_CASE("TrackView - Custom Empty Key And Value", "[library][unit][track]")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"", ""}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    std::int32_t count = 0;

    for (auto const& [k, v] : view.customMetadata())
    {
      CHECK(k == DictionaryId{1});
      CHECK(v.empty());
      ++count;
    }

    CHECK(count == 1);
  }

  TEST_CASE("TrackView - Custom Special Characters In Value", "[library][unit][track]")
  {
    auto const pairs = std::vector{std::pair<std::string, std::string>{"comment", "Hello, World! 你好"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    for (auto const& [k, v] : view.customMetadata())
    {
      CHECK(k == DictionaryId{1});
      CHECK(v == "Hello, World! 你好");
    }
  }

  // === View Validity Tests ===
  TEST_CASE("TrackView - Hot Valid", "[library][unit][track]")
  {
    auto const data = createMinimalHotData();
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.isHotValid() == true);
  }

  TEST_CASE("TrackView - Hot Invalid Null Data", "[library][unit][track]")
  {
    auto const nullSpan = std::span<std::byte const>{static_cast<std::byte const*>(nullptr), 100};
    auto const nullView = TrackView{nullSpan, std::span<std::byte const>{}};
    CHECK(nullView.isHotValid() == false);
  }

  TEST_CASE("TrackView - Hot Invalid Too Small", "[library][unit][track]")
  {
    auto const smallData = std::array<char, 10>{};
    auto const smallView = TrackView{utility::bytes::view(smallData), std::span<std::byte const>{}};
    CHECK(smallView.isHotValid() == false);
  }

  TEST_CASE("TrackView - Cold Valid", "[library][unit][track]")
  {
    auto const data = createColdData();
    auto const view = TrackView{std::span<std::byte const>{}, data};
    CHECK(view.isColdValid() == true);
  }

  TEST_CASE("TrackView - Cold Invalid Null Data", "[library][unit][track]")
  {
    auto const nullSpan = std::span<std::byte const>{static_cast<std::byte const*>(nullptr), 100};
    auto const nullView = TrackView{std::span<std::byte const>{}, nullSpan};
    CHECK(nullView.isColdValid() == false);
  }

  TEST_CASE("TrackView - Cold Invalid Too Small", "[library][unit][track]")
  {
    auto const smallData = std::array<char, 10>{};
    auto const smallView = TrackView{std::span<std::byte const>{}, utility::bytes::view(smallData)};
    CHECK(smallView.isColdValid() == false);
  }
} // namespace ao::library::test
