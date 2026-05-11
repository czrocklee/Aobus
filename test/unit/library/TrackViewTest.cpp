// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackView.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/utility/ByteView.h>
#include <test/unit/library/TestUtils.h>
#include <test/unit/lmdb/TestUtils.h>

#include <array>
#include <ranges>
#include <span>
#include <vector>

namespace
{
#if defined(__GNUC__) && !defined(__clang__)
  static_assert(std::ranges::view<ao::library::TrackView::TagProxy>);
  static_assert(std::ranges::view<ao::library::TrackView::CustomProxy>);
#endif

  using namespace test;
  using ao::DictionaryId;
  using ao::TrackId;
  using ao::library::TrackHotHeader;
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

  using ao::library::TrackColdHeader;

  std::vector<std::byte> createColdData(TrackColdHeader const& header = {},
                                        std::vector<std::pair<std::string, std::string>> const& customPairs = {},
                                        std::string_view uri = "")
  {
    auto builder = ao::library::TrackBuilder::createNew();
    builder.property().uri(uri);
    builder.property().fileSize(ao::utility::uint64Parts::combine(header.fileSizeLo, header.fileSizeHi));
    builder.property().mtime(ao::utility::uint64Parts::combine(header.mtimeLo, header.mtimeHi));
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
    auto env = ao::lmdb::Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = ao::lmdb::WriteTransaction{env};
    auto dict = ao::library::DictionaryStore{ao::lmdb::Database{wtxn, "dict"}, wtxn};
    auto resources = ao::library::ResourceStore{ao::lmdb::Database{wtxn, "resources"}};
    return builder.serializeCold(wtxn, dict, resources);
  }

  ao::library::TrackView makeColdView(std::vector<std::byte> const& data)
  {
    return ao::library::TrackView{std::span<std::byte const>{}, data};
  }

  // === Metadata Tests ===

  TEST_CASE("TrackView - Hot Title")
  {
    auto data = createTrackWithStrings("Test Title");
    auto view = ao::library::TrackView{data, std::span<std::byte const>{}};
    CHECK(view.metadata().title() == "Test Title");
  }

  TEST_CASE("TrackView - Hot Title Empty")
  {
    auto data = createMinimalHotData();
    auto view = ao::library::TrackView{data, std::span<std::byte const>{}};
    CHECK(view.metadata().title().empty());
  }

  TEST_CASE("TrackView - Hot Dictionary IDs")
  {
    auto data = createMinimalHotData();
    auto view = ao::library::TrackView{data, std::span<std::byte const>{}};

    CHECK(view.metadata().artistId() == DictionaryId{1});
    CHECK(view.metadata().albumId() == DictionaryId{2});
    CHECK(view.metadata().genreId() == DictionaryId{3});
    CHECK(view.metadata().albumArtistId() == DictionaryId{0});
    CHECK(view.metadata().composerId() == DictionaryId{0});
  }

  TEST_CASE("TrackView - Hot Year")
  {
    auto data = createMinimalHotData();
    auto view = ao::library::TrackView{data, std::span<std::byte const>{}};
    CHECK(view.metadata().year() == 2020);
  }

  TEST_CASE("TrackView - Cold Track Info")
  {
    auto header = TrackColdHeader{};
    header.trackNumber = 5;
    header.totalTracks = 10;
    header.discNumber = 1;
    header.totalDiscs = 2;

    auto data = createColdData(header, {}, "/path/to/file.flac");
    auto view = makeColdView(data);

    CHECK(view.metadata().trackNumber() == 5);
    CHECK(view.metadata().totalTracks() == 10);
    CHECK(view.metadata().discNumber() == 1);
    CHECK(view.metadata().totalDiscs() == 2);
  }

  TEST_CASE("TrackView - Cold Cover Art")
  {
    auto header = TrackColdHeader{};
    header.coverArtId = 42;

    auto data = createColdData(header, {}, "");
    auto view = makeColdView(data);

    CHECK(view.metadata().coverArtId() == 42);
  }

  TEST_CASE("TrackView - Cold Uri")
  {
    auto data = createColdData({}, {}, "/path/to/file.flac");
    auto view = makeColdView(data);

    CHECK(view.metadata().coverArtId() == 0);
    CHECK(view.property().uri() == "/path/to/file.flac");
  }

  TEST_CASE("TrackView - Cold Uri Empty")
  {
    auto data = createColdData({}, {}, "");
    auto view = makeColdView(data);

    CHECK(view.property().uri().empty());
  }

  // === Property Tests ===

  TEST_CASE("TrackView - Hot Codec and BitDepth")
  {
    auto h = TrackHotHeader{};
    h.codecId = 3; // FLAC
    h.bitDepth = 24;
    h.rating = 0;
    auto data = serializeHeader(h);
    auto view = ao::library::TrackView{data, std::span<std::byte const>{}};

    CHECK(view.property().codecId() == 3);
    CHECK(view.property().bitDepth() == 24);
  }

  TEST_CASE("TrackView - Hot Rating")
  {
    auto data = createMinimalHotData();
    auto view = ao::library::TrackView{data, std::span<std::byte const>{}};

    CHECK(view.metadata().rating() == 3);
  }

  TEST_CASE("TrackView - Hot Rating Boundary")
  {
    auto h = TrackHotHeader{};
    h.rating = 0;
    auto data = serializeHeader(h);
    CHECK(ao::library::TrackView{data, std::span<std::byte const>{}}.metadata().rating() == 0);

    h.rating = 255;
    data = serializeHeader(h);
    CHECK(ao::library::TrackView{data, std::span<std::byte const>{}}.metadata().rating() == 255);
  }

  TEST_CASE("TrackView - Cold File Size and Mtime")
  {
    auto header = TrackColdHeader{};
    std::tie(header.fileSizeLo, header.fileSizeHi) = split(12345678);
    std::tie(header.mtimeLo, header.mtimeHi) = split(987654321);

    auto data = createColdData(header, {}, "");
    auto view = makeColdView(data);

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

    auto data = createColdData(header, {}, "");
    auto view = makeColdView(data);

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

    auto data = serializeHeader(h);
    auto view = ao::library::TrackView{data, std::span<std::byte const>{}};
    CHECK(view.tags().bloom() == 0xCAFE);
  }

  TEST_CASE("TrackView - Tag Count Zero")
  {
    auto data = createTrackWithStrings("Test");
    auto view = ao::library::TrackView{data, std::span<std::byte const>{}};

    CHECK(view.tags().count() == 0);
  }

  TEST_CASE("TrackView - Tag Iterator Empty")
  {
    auto data = createTrackWithStrings("Test");
    auto view = ao::library::TrackView{data, std::span<std::byte const>{}};

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

    std::string title = "Test Title";
    h.titleLen = static_cast<std::uint16_t>(title.size());

    auto data = serializeHeader(h);

    std::uint32_t tag1 = 10;
    std::uint32_t tag2 = 20;
    data.insert_range(data.end(), ao::utility::bytes::view(tag1));
    data.insert_range(data.end(), ao::utility::bytes::view(tag2));

    appendString(data, title);

    auto view = ao::library::TrackView{data, std::span<std::byte const>{}};
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

    std::string title = "Test Title";
    h.titleLen = static_cast<std::uint16_t>(title.size());

    auto data = serializeHeader(h);

    std::uint32_t tag1 = 10;
    std::uint32_t tag2 = 20;
    data.insert_range(data.end(), ao::utility::bytes::view(tag1));
    data.insert_range(data.end(), ao::utility::bytes::view(tag2));

    appendString(data, title);

    auto view = ao::library::TrackView{data, std::span<std::byte const>{}};

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
    std::uint32_t tag1 = 10;
    std::uint32_t tag2 = 20;
    data.insert_range(data.end(), ao::utility::bytes::view(tag1));
    data.insert_range(data.end(), ao::utility::bytes::view(tag2));

    auto view = ao::library::TrackView{data, std::span<std::byte const>{}};

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
    std::uint32_t tag1 = 10;
    std::uint32_t tag2 = 20;
    data.insert_range(data.end(), ao::utility::bytes::view(tag1));
    data.insert_range(data.end(), ao::utility::bytes::view(tag2));

    auto view = ao::library::TrackView{data, std::span<std::byte const>{}};

    CHECK(view.tags().id(0) == DictionaryId{10});
    CHECK(view.tags().id(1) == DictionaryId{20});
  }

  // === Custom Tests ===

  TEST_CASE("TrackView - Custom Roundtrip Empty")
  {
    auto data = createColdData();
    auto view = makeColdView(data);

    int count = 0;
    for ([[maybe_unused]] auto const& [k, v] : view.custom())
    {
      ++count;
    }

    CHECK(count == 0);
  }

  TEST_CASE("TrackView - Custom Roundtrip Single Pair")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"key1", "value1"}};
    auto data = createColdData({}, pairs, "/path/to/file.flac");
    auto view = makeColdView(data);

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
    auto key1 = std::string{"replaygain_track_gain_db"};
    auto key2 = std::string{"isrc"};
    auto key3 = std::string{"edition"};
    auto pairs =
      std::vector<std::pair<std::string, std::string>>{{key1, "-6.5"}, {key2, "USSM19999999"}, {key3, "remaster"}};

    auto data = createColdData({}, pairs, "/path/to/file.flac");
    auto view = makeColdView(data);

    // DictionaryStore assigns sequential IDs: key1->1, key2->2, key3->3
    auto id0 = DictionaryId{1};
    auto id1 = DictionaryId{2};
    auto id2 = DictionaryId{3};

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
    auto data = createColdData({}, {}, "");
    auto view = makeColdView(data);

    int count = 0;
    for ([[maybe_unused]] auto [key, value] : view.custom())
    {
      ++count;
    }

    CHECK(count == 0);
  }

  TEST_CASE("TrackView - Custom Iterator Single Pair")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"key1", "value1"}};

    auto data = createColdData({}, pairs, "");
    auto view = makeColdView(data);

    int count = 0;
    for (auto [key, value] : view.custom())
    {
      CHECK(key == DictionaryId{1});
      CHECK(value == "value1");
      ++count;
    }

    CHECK(count == 1);
  }

  TEST_CASE("TrackView - Custom Iterator Special Characters")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"comment", "Hello, World! 你好"}};

    auto data = createColdData({}, pairs, "");
    auto view = makeColdView(data);

    for (auto [key, value] : view.custom())
    {
      CHECK(key == DictionaryId{1});
      CHECK(value == "Hello, World! 你好");
    }
  }

  TEST_CASE("TrackView - Custom Iterator Multiple Pairs")
  {
    auto key1 = std::string{"replaygain_track_gain_db"};
    auto key2 = std::string{"isrc"};
    auto key3 = std::string{"edition"};
    auto pairs =
      std::vector<std::pair<std::string, std::string>>{{key1, "-6.5"}, {key2, "USSM19999999"}, {key3, "remaster"}};

    auto data = createColdData({}, pairs, "");
    auto view = makeColdView(data);

    auto result = std::vector<std::pair<DictionaryId, std::string_view>>{};
    for (auto [key, value] : view.custom())
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
    auto pairs =
      std::vector<std::pair<std::string, std::string>>{{"replaygain_track_gain_db", "-6.5"}, {"isrc", "USSM19999999"}};

    auto data = createColdData({}, pairs, "");
    auto view = makeColdView(data);

    // DictionaryStore assigns: replaygain->1, isrc->2
    auto value = view.custom().get(DictionaryId{2});
    CHECK(value.has_value() == true);
    CHECK(*value == "USSM19999999");
  }

  TEST_CASE("TrackView - Custom Get Not Found")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"replaygain_track_gain_db", "-6.5"}};

    auto data = createColdData({}, pairs, "");
    auto view = makeColdView(data);

    // ID 99 was never assigned
    auto value = view.custom().get(DictionaryId{99});
    CHECK(value.has_value() == false);
  }

  TEST_CASE("TrackView - Custom Get Case Sensitive")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"ISRC", "USSM19999999"}};

    auto data = createColdData({}, pairs, "");
    auto view = makeColdView(data);

    // "ISRC" is stored at ID 1, looking up by ID 1 returns the value
    auto value = view.custom().get(DictionaryId{1});
    CHECK(value.has_value() == true);
    CHECK(*value == "USSM19999999");
  }

  TEST_CASE("TrackView - Custom Get Binary Search Path (64+ entries)")
  {
    // Create 100 entries to force binary search path
    auto pairs = std::vector<std::pair<std::string, std::string>>{};

    for (int i = 0; i < 100; ++i)
    {
      pairs.emplace_back(std::format("key{}", i), std::format("value{}", i));
    }

    auto data = createColdData({}, pairs, "");
    auto view = makeColdView(data);

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
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"", ""}};

    auto data = createColdData({}, pairs, "");
    auto view = makeColdView(data);

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
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"comment", "Hello, World! 你好"}};

    auto data = createColdData({}, pairs, "");
    auto view = makeColdView(data);

    for (auto const& [k, v] : view.custom())
    {
      CHECK(k == DictionaryId{1});
      CHECK(v == "Hello, World! 你好");
    }
  }

  // === View Validity Tests ===

  TEST_CASE("TrackView - Hot Valid")
  {
    auto data = createMinimalHotData();
    auto view = ao::library::TrackView{data, std::span<std::byte const>{}};
    CHECK(view.isHotValid() == true);
  }

  TEST_CASE("TrackView - Hot Invalid Null Data")
  {
    auto nullSpan = std::span<std::byte const>{static_cast<std::byte const*>(nullptr), 100};
    ao::library::TrackView nullView{nullSpan, std::span<std::byte const>{}};
    CHECK(nullView.isHotValid() == false);
  }

  TEST_CASE("TrackView - Hot Invalid Too Small")
  {
    auto smallData = std::array<char, 10>{};
    auto smallView = ao::library::TrackView{ao::utility::bytes::view(smallData), std::span<std::byte const>{}};
    CHECK(smallView.isHotValid() == false);
  }

  TEST_CASE("TrackView - Cold Valid")
  {
    auto data = createColdData();
    ao::library::TrackView view{std::span<std::byte const>{}, data};
    CHECK(view.isColdValid() == true);
  }

  TEST_CASE("TrackView - Cold Invalid Null Data")
  {
    auto nullSpan = std::span<std::byte const>{static_cast<std::byte const*>(nullptr), 100};
    ao::library::TrackView nullView{std::span<std::byte const>{}, nullSpan};
    CHECK(nullView.isColdValid() == false);
  }

  TEST_CASE("TrackView - Cold Invalid Too Small")
  {
    auto smallData = std::array<char, 10>{};
    auto smallView = ao::library::TrackView{std::span<std::byte const>{}, ao::utility::bytes::view(smallData)};
    CHECK(smallView.isColdValid() == false);
  }
} // anonymous namespace
