// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>
#include <lmdb.h>
#include <test/unit/library/TestUtils.h>
#include <test/unit/lmdb/TestUtils.h>

#include <array>
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
    static_assert(std::ranges::view<TrackView::CustomProxy>);
#endif

    using namespace ao::lmdb::test;
    using ao::utility::uint64Parts::split;

    // Helper to create a minimal valid hot TrackView for testing
    std::vector<std::byte> createMinimalHotData()
    {
      auto h = TrackHotHeader{};
      h.tagBloom = 0;
      h.artistId = DictionaryId{1};
      h.albumId = DictionaryId{2};
      h.genreId = DictionaryId{3};
      h.albumArtistId = DictionaryId{0};
      h.composerId = DictionaryId{0};
      h.year = 2020;
      h.codecId = 0;
      h.bitDepth = 16;
      h.rating = 3;
      h.tagLen = 0;
      h.titleLen = 0;

      return serializeHeader(h);
    }

    std::vector<std::byte> createTrackWithStrings(std::string_view title)
    {
      auto h = TrackHotHeader{};
      h.tagBloom = 0;
      h.artistId = DictionaryId{1};
      h.albumId = DictionaryId{2};
      h.genreId = DictionaryId{3};
      h.albumArtistId = DictionaryId{0};
      h.composerId = DictionaryId{0};
      h.year = 2020;
      h.codecId = 0;
      h.bitDepth = 16;
      h.rating = 3;
      h.tagLen = 0; // no tags

      // In new layout: tags first (at sizeof(TrackHotHeader)), title after (at sizeof(TrackHotHeader) + tagLen)
      h.titleLen = static_cast<std::uint16_t>(title.size());

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
      builder.property().fileSize(utility::uint64Parts::combine(header.fileSizeLo, header.fileSizeHi));
      builder.property().mtime(utility::uint64Parts::combine(header.mtimeLo, header.mtimeHi));
      builder.metadata().coverArtId(header.coverArtId);
      builder.metadata().trackNumber(header.trackNumber);
      builder.metadata().totalTracks(header.totalTracks);
      builder.metadata().discNumber(header.discNumber);
      builder.metadata().totalDiscs(header.totalDiscs);
      builder.property().durationMs(header.durationMs);
      builder.property().sampleRate(header.sampleRate);
      builder.property().bitrate(header.bitrate);
      builder.property().channels(header.channels);

      for (auto const& [key, value] : customPairs)
      {
        builder.custom().add(key, value);
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
  TEST_CASE("TrackView - Hot Title")
  {
    auto const data = createTrackWithStrings("Test Title");
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.metadata().title() == "Test Title");
  }

  TEST_CASE("TrackView - Hot Title Empty")
  {
    auto const data = createMinimalHotData();
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.metadata().title().empty());
  }

  TEST_CASE("TrackView - Hot Dictionary IDs")
  {
    auto const data = createMinimalHotData();
    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.metadata().artistId() == DictionaryId{1});
    CHECK(view.metadata().albumId() == DictionaryId{2});
    CHECK(view.metadata().genreId() == DictionaryId{3});
    CHECK(view.metadata().albumArtistId() == DictionaryId{0});
    CHECK(view.metadata().composerId() == DictionaryId{0});
  }

  TEST_CASE("TrackView - Hot Year")
  {
    auto const data = createMinimalHotData();
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.metadata().year() == 2020);
  }

  TEST_CASE("TrackView - Cold Track Info")
  {
    auto header = TrackColdHeader{};
    header.trackNumber = 5;
    header.totalTracks = 10;
    header.discNumber = 1;
    header.totalDiscs = 2;

    auto const data = createColdData(header, {}, "/path/to/file.flac");
    auto const view = makeColdView(data);

    CHECK(view.metadata().trackNumber() == 5);
    CHECK(view.metadata().totalTracks() == 10);
    CHECK(view.metadata().discNumber() == 1);
    CHECK(view.metadata().totalDiscs() == 2);
  }

  TEST_CASE("TrackView - Cold Cover Art")
  {
    auto header = TrackColdHeader{};
    header.coverArtId = 42;

    auto const data = createColdData(header, {}, "");
    auto const view = makeColdView(data);

    CHECK(view.metadata().coverArtId() == 42);
  }

  TEST_CASE("TrackView - Cold Uri")
  {
    auto const data = createColdData({}, {}, "/path/to/file.flac");
    auto const view = makeColdView(data);

    CHECK(view.metadata().coverArtId() == 0);
    CHECK(view.property().uri() == "/path/to/file.flac");
  }

  TEST_CASE("TrackView - Cold Uri Empty")
  {
    auto const data = createColdData({}, {}, "");
    auto const view = makeColdView(data);

    CHECK(view.property().uri().empty());
  }

  // === Property Tests ===
  TEST_CASE("TrackView - Hot Codec and BitDepth")
  {
    auto h = TrackHotHeader{};
    h.codecId = 3; // FLAC
    h.bitDepth = 24;
    h.rating = 0;
    auto const data = serializeHeader(h);
    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.property().codecId() == 3);
    CHECK(view.property().bitDepth() == 24);
  }

  TEST_CASE("TrackView - Hot Rating")
  {
    auto const data = createMinimalHotData();
    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.metadata().rating() == 3);
  }

  TEST_CASE("TrackView - Hot Rating Boundary")
  {
    auto h = TrackHotHeader{};
    h.rating = 0;
    auto data = serializeHeader(h);
    CHECK(TrackView{data, std::span<std::byte const>{}}.metadata().rating() == 0);

    h.rating = 255;
    data = serializeHeader(h);
    CHECK(TrackView{data, std::span<std::byte const>{}}.metadata().rating() == 255);
  }

  TEST_CASE("TrackView - Cold File Size and Mtime")
  {
    auto header = TrackColdHeader{};
    std::tie(header.fileSizeLo, header.fileSizeHi) = split(12345678);
    std::tie(header.mtimeLo, header.mtimeHi) = split(987654321);

    auto const data = createColdData(header, {}, "");
    auto const view = makeColdView(data);

    CHECK(view.property().fileSize() == 12345678);
    CHECK(view.property().mtime() == 987654321);
  }

  TEST_CASE("TrackView - Cold Audio Format")
  {
    auto header = TrackColdHeader{};
    header.durationMs = 180000;
    header.sampleRate = 44100;
    header.bitrate = 320000;
    header.channels = 2;

    auto const data = createColdData(header, {}, "");
    auto const view = makeColdView(data);

    CHECK(view.property().durationMs() == 180000);
    CHECK(view.property().sampleRate() == 44100);
    CHECK(view.property().bitrate() == 320000);
    CHECK(view.property().channels() == 2);
  }

  // === Tag Tests ===
  TEST_CASE("TrackView - Bloom Filter")
  {
    auto h = TrackHotHeader{};
    h.tagBloom = 0xCAFE;

    auto const data = serializeHeader(h);
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.tags().bloom() == 0xCAFE);
  }

  TEST_CASE("TrackView - Tag Count Zero")
  {
    auto const data = createTrackWithStrings("Test");
    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.tags().count() == 0);
  }

  TEST_CASE("TrackView - Tag Iterator Empty")
  {
    auto const data = createTrackWithStrings("Test");
    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.tags().begin() == view.tags().end());
  }

  TEST_CASE("TrackView - Tag Count With Tags")
  {
    auto h = TrackHotHeader{};
    h.tagBloom = 0;
    h.artistId = DictionaryId{1};
    h.albumId = DictionaryId{2};
    h.genreId = DictionaryId{3};
    h.albumArtistId = DictionaryId{0};
    h.composerId = DictionaryId{0};
    h.year = 2020;
    h.codecId = 0;
    h.bitDepth = 16;
    h.rating = 3;
    h.tagLen = 8; // 2 tags * 4 bytes

    auto const title = std::string{"Test Title"};
    h.titleLen = static_cast<std::uint16_t>(title.size());

    auto data = serializeHeader(h);

    auto const tag1 = std::uint32_t{10};
    auto const tag2 = std::uint32_t{20};
    data.insert_range(data.end(), utility::bytes::view(tag1));
    data.insert_range(data.end(), utility::bytes::view(tag2));

    appendString(data, title);

    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.tags().count() == 2);
  }

  TEST_CASE("TrackView - Tag Iterator")
  {
    auto h = TrackHotHeader{};
    h.tagBloom = 0;
    h.artistId = DictionaryId{1};
    h.albumId = DictionaryId{2};
    h.genreId = DictionaryId{3};
    h.albumArtistId = DictionaryId{0};
    h.composerId = DictionaryId{0};
    h.year = 2020;
    h.codecId = 0;
    h.bitDepth = 16;
    h.rating = 3;
    h.tagLen = 8; // 2 tags * 4 bytes

    auto const title = std::string{"Test Title"};
    h.titleLen = static_cast<std::uint16_t>(title.size());

    auto data = serializeHeader(h);

    auto const tag1 = std::uint32_t{10};
    auto const tag2 = std::uint32_t{20};
    data.insert_range(data.end(), utility::bytes::view(tag1));
    data.insert_range(data.end(), utility::bytes::view(tag2));

    appendString(data, title);

    auto const view = TrackView{data, std::span<std::byte const>{}};

    std::vector<DictionaryId> ids(view.tags().begin(), view.tags().end());
    CHECK(ids.size() == 2);
    CHECK(ids[0] == DictionaryId{10});
    CHECK(ids[1] == DictionaryId{20});
  }

  TEST_CASE("TrackView - Tag Has")
  {
    auto h = TrackHotHeader{};
    h.tagBloom = 0;
    h.artistId = DictionaryId{1};
    h.albumId = DictionaryId{2};
    h.genreId = DictionaryId{3};
    h.albumArtistId = DictionaryId{0};
    h.composerId = DictionaryId{0};
    h.year = 2020;
    h.codecId = 0;
    h.bitDepth = 16;
    h.rating = 3;
    h.tagLen = 8;

    auto data = serializeHeader(h);
    auto const tag1 = std::uint32_t{10};
    auto const tag2 = std::uint32_t{20};
    data.insert_range(data.end(), utility::bytes::view(tag1));
    data.insert_range(data.end(), utility::bytes::view(tag2));

    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.tags().has(DictionaryId{10}) == true);
    CHECK(view.tags().has(DictionaryId{20}) == true);
    CHECK(view.tags().has(DictionaryId{30}) == false);
  }

  TEST_CASE("TrackView - Tag Id Accessor")
  {
    auto h = TrackHotHeader{};
    h.tagBloom = 0;
    h.artistId = DictionaryId{1};
    h.albumId = DictionaryId{2};
    h.genreId = DictionaryId{3};
    h.albumArtistId = DictionaryId{0};
    h.composerId = DictionaryId{0};
    h.year = 2020;
    h.codecId = 0;
    h.bitDepth = 16;
    h.rating = 3;
    h.tagLen = 8;

    auto data = serializeHeader(h);
    auto const tag1 = std::uint32_t{10};
    auto const tag2 = std::uint32_t{20};
    data.insert_range(data.end(), utility::bytes::view(tag1));
    data.insert_range(data.end(), utility::bytes::view(tag2));

    auto const view = TrackView{data, std::span<std::byte const>{}};

    CHECK(view.tags().id(0) == DictionaryId{10});
    CHECK(view.tags().id(1) == DictionaryId{20});
  }

  // === Custom Tests ===
  TEST_CASE("TrackView - Custom Roundtrip Empty")
  {
    auto const data = createColdData();
    auto const view = makeColdView(data);

    int count = 0;

    for ([[maybe_unused]] auto const& [k, v] : view.custom())
    {
      ++count;
    }

    CHECK(count == 0);
  }

  TEST_CASE("TrackView - Custom Roundtrip Single Pair")
  {
    auto const pairs = std::vector<std::pair<std::string, std::string>>{{"key1", "value1"}};
    auto const data = createColdData({}, pairs, "/path/to/file.flac");
    auto const view = makeColdView(data);

    int count = 0;

    for (auto const& [k, v] : view.custom())
    {
      CHECK(k == DictionaryId{1});
      CHECK(v == "value1");
      ++count;
    }

    CHECK(count == 1);
    CHECK(view.property().uri() == "/path/to/file.flac");
  }

  TEST_CASE("TrackView - Custom Roundtrip Multiple Pairs")
  {
    auto const key1 = std::string{"replaygain_track_gain_db"};
    auto const key2 = std::string{"isrc"};
    auto const key3 = std::string{"edition"};
    auto const pairs =
      std::vector<std::pair<std::string, std::string>>{{key1, "-6.5"}, {key2, "USSM19999999"}, {key3, "remaster"}};

    auto const data = createColdData({}, pairs, "/path/to/file.flac");
    auto const view = makeColdView(data);

    // DictionaryStore assigns sequential IDs: key1->1, key2->2, key3->3
    auto const id0 = DictionaryId{1};
    auto const id1 = DictionaryId{2};
    auto const id2 = DictionaryId{3};

    auto result = std::vector<std::pair<DictionaryId, std::string_view>>{};

    for (auto const& [k, v] : view.custom())
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

  TEST_CASE("TrackView - Custom Iterator Empty")
  {
    auto const data = createColdData({}, {}, "");
    auto const view = makeColdView(data);

    int count = 0;

    for ([[maybe_unused]] auto const& [key, value] : view.custom())
    {
      ++count;
    }

    CHECK(count == 0);
  }

  TEST_CASE("TrackView - Custom Iterator Single Pair")
  {
    auto const pairs = std::vector<std::pair<std::string, std::string>>{{"key1", "value1"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    int count = 0;

    for (auto const& [key, value] : view.custom())
    {
      CHECK(key == DictionaryId{1});
      CHECK(value == "value1");
      ++count;
    }

    CHECK(count == 1);
  }

  TEST_CASE("TrackView - Custom Iterator Special Characters")
  {
    auto const pairs = std::vector<std::pair<std::string, std::string>>{{"comment", "Hello, World! 你好"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    for (auto const& [key, value] : view.custom())
    {
      CHECK(key == DictionaryId{1});
      CHECK(value == "Hello, World! 你好");
    }
  }

  TEST_CASE("TrackView - Custom Iterator Multiple Pairs")
  {
    auto const key1 = std::string{"replaygain_track_gain_db"};
    auto const key2 = std::string{"isrc"};
    auto const key3 = std::string{"edition"};
    auto const pairs =
      std::vector<std::pair<std::string, std::string>>{{key1, "-6.5"}, {key2, "USSM19999999"}, {key3, "remaster"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    auto result = std::vector<std::pair<DictionaryId, std::string_view>>{};

    for (auto const& [key, value] : view.custom())
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

  TEST_CASE("TrackView - Custom Get Found")
  {
    auto const pairs =
      std::vector<std::pair<std::string, std::string>>{{"replaygain_track_gain_db", "-6.5"}, {"isrc", "USSM19999999"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    // DictionaryStore assigns: replaygain->1, isrc->2
    auto const optValue = view.custom().get(DictionaryId{2});
    CHECK(optValue.has_value() == true);
    CHECK(*optValue == "USSM19999999");
  }

  TEST_CASE("TrackView - Custom Get Not Found")
  {
    auto const pairs = std::vector<std::pair<std::string, std::string>>{{"replaygain_track_gain_db", "-6.5"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    // ID 99 was never assigned
    auto const optValue = view.custom().get(DictionaryId{99});
    CHECK(optValue.has_value() == false);
  }

  TEST_CASE("TrackView - Custom Get Case Sensitive")
  {
    auto const pairs = std::vector<std::pair<std::string, std::string>>{{"ISRC", "USSM19999999"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    // "ISRC" is stored at ID 1, looking up by ID 1 returns the value
    auto const optValue = view.custom().get(DictionaryId{1});
    CHECK(optValue.has_value() == true);
    CHECK(*optValue == "USSM19999999");
  }

  TEST_CASE("TrackView - Custom Get Binary Search Path (64+ entries)")
  {
    // Create 100 entries to force binary search path
    auto pairs = std::vector<std::pair<std::string, std::string>>{};

    for (int i = 0; i < 100; ++i)
    {
      pairs.emplace_back(std::format("key{}", i), std::format("value{}", i));
    }

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    // First entry (dictId=1)
    CHECK(view.custom().get(DictionaryId{1}).value() == "value0");
    // Middle entry (dictId=50)
    CHECK(view.custom().get(DictionaryId{50}).value() == "value49");
    // Last entry (dictId=100)
    CHECK(view.custom().get(DictionaryId{100}).value() == "value99");
    // Not found
    CHECK(view.custom().get(DictionaryId{199}).has_value() == false);
    // Before first (0 = null, should throw)
    CHECK(view.custom().get(DictionaryId{0}).has_value() == false);
  }

  TEST_CASE("TrackView - Custom Empty Key And Value")
  {
    auto const pairs = std::vector<std::pair<std::string, std::string>>{{"", ""}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    int count = 0;

    for (auto const& [k, v] : view.custom())
    {
      CHECK(k == DictionaryId{1});
      CHECK(v.empty());
      ++count;
    }

    CHECK(count == 1);
  }

  TEST_CASE("TrackView - Custom Special Characters In Value")
  {
    auto const pairs = std::vector<std::pair<std::string, std::string>>{{"comment", "Hello, World! 你好"}};

    auto const data = createColdData({}, pairs, "");
    auto const view = makeColdView(data);

    for (auto const& [k, v] : view.custom())
    {
      CHECK(k == DictionaryId{1});
      CHECK(v == "Hello, World! 你好");
    }
  }

  // === View Validity Tests ===
  TEST_CASE("TrackView - Hot Valid")
  {
    auto const data = createMinimalHotData();
    auto const view = TrackView{data, std::span<std::byte const>{}};
    CHECK(view.isHotValid() == true);
  }

  TEST_CASE("TrackView - Hot Invalid Null Data")
  {
    auto const nullSpan = std::span<std::byte const>{static_cast<std::byte const*>(nullptr), 100};
    auto const nullView = TrackView{nullSpan, std::span<std::byte const>{}};
    CHECK(nullView.isHotValid() == false);
  }

  TEST_CASE("TrackView - Hot Invalid Too Small")
  {
    auto const smallData = std::array<char, 10>{};
    auto const smallView = TrackView{utility::bytes::view(smallData), std::span<std::byte const>{}};
    CHECK(smallView.isHotValid() == false);
  }

  TEST_CASE("TrackView - Cold Valid")
  {
    auto const data = createColdData();
    auto const view = TrackView{std::span<std::byte const>{}, data};
    CHECK(view.isColdValid() == true);
  }

  TEST_CASE("TrackView - Cold Invalid Null Data")
  {
    auto const nullSpan = std::span<std::byte const>{static_cast<std::byte const*>(nullptr), 100};
    auto const nullView = TrackView{std::span<std::byte const>{}, nullSpan};
    CHECK(nullView.isColdValid() == false);
  }

  TEST_CASE("TrackView - Cold Invalid Too Small")
  {
    auto const smallData = std::array<char, 10>{};
    auto const smallView = TrackView{std::span<std::byte const>{}, utility::bytes::view(smallData)};
    CHECK(smallView.isColdValid() == false);
  }
} // namespace ao::library::test
