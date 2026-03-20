// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <cstddef>
#include <rs/core/TrackRecord.h>
#include <rs/core/TrackView.h>
#include <span>
#include <test/core/TestUtils.h>

#include <vector>

namespace
{
  using namespace test;
  using rs::core::DictionaryId;
  using rs::core::TrackHotHeader;
  using rs::core::TrackId;
  using rs::utility::splitInt64;

  // Helper to create a minimal valid hot TrackView for testing
  std::vector<std::byte> createMinimalHotData()
  {
    TrackHotHeader h{};
    h.tagBloom = 0;
    h.artistId = DictionaryId{1};
    h.albumId = DictionaryId{2};
    h.genreId = DictionaryId{3};
    h.albumArtistId = DictionaryId{0};
    h.year = 2020;
    h.codecId = 0;
    h.bitDepth = 16;
    h.rating = 3;
    h.tagCount = 0;
    h.titleOffset = 0;
    h.titleLen = 0;
    h.tagsOffset = 0;

    return serializeHeader(h);
  }

  std::vector<std::byte> createTrackWithStrings(std::string_view title)
  {
    TrackHotHeader h{};
    h.tagBloom = 0;
    h.artistId = DictionaryId{1};
    h.albumId = DictionaryId{2};
    h.genreId = DictionaryId{3};
    h.albumArtistId = DictionaryId{0};
    h.year = 2020;
    h.codecId = 0;
    h.bitDepth = 16;
    h.rating = 3;
    h.tagCount = 0;

    // Title at offset 0 in payload, tags after title + null terminator
    h.titleOffset = 0;
    h.titleLen = static_cast<std::uint16_t>(title.size());
    h.tagsOffset = static_cast<std::uint16_t>(title.size() + 1);

    auto data = serializeHeader(h);

    // Add title + null
    appendString(data, title);

    return data;
  }

  TEST_CASE("TrackHotHeader - Size and Alignment")
  {
    CHECK(sizeof(TrackHotHeader) == 36);
    CHECK(alignof(TrackHotHeader) == 4);
  }

  TEST_CASE("TrackHotHeader - Field Offsets")
  {
    // Verify key field offsets for ABI compatibility
    // Check 4-byte section
    CHECK(offsetof(TrackHotHeader, tagBloom) == 0);

    // Check 4-byte section continues
    CHECK(offsetof(TrackHotHeader, artistId) == 4);
    CHECK(offsetof(TrackHotHeader, albumId) == 8);
    CHECK(offsetof(TrackHotHeader, genreId) == 12);
    CHECK(offsetof(TrackHotHeader, albumArtistId) == 16);

    // Check 2-byte section starts at offset 20
    CHECK(offsetof(TrackHotHeader, year) == 20);
    CHECK(offsetof(TrackHotHeader, codecId) == 22);
    CHECK(offsetof(TrackHotHeader, bitDepth) == 24);
    CHECK(offsetof(TrackHotHeader, titleOffset) == 26);
    CHECK(offsetof(TrackHotHeader, titleLen) == 28);
    CHECK(offsetof(TrackHotHeader, tagsOffset) == 30);

    // Check 1-byte section
    CHECK(offsetof(TrackHotHeader, rating) == 32);
    CHECK(offsetof(TrackHotHeader, tagCount) == 33);
  }

  TEST_CASE("TrackView (Hot) - Empty View")
  {
    // Create an "empty" view by using empty hot data
    rs::core::TrackView view{TrackId{0}, std::span<std::byte const>{}, std::nullopt, nullptr};
    CHECK(view.isHotValid() == false);
    CHECK(view.hasCold() == false);
  }

  TEST_CASE("TrackView (Hot) - Construct from Hot Data")
  {
    auto data = createMinimalHotData();
    rs::core::TrackView view{TrackId{0}, std::as_bytes(std::span{data}), std::nullopt, nullptr};

    CHECK(view.isHotValid() == true);
    CHECK(view.hotHeader() != nullptr);
  }

  TEST_CASE("TrackView (Hot) - Fixed Field Accessors")
  {
    auto data = createMinimalHotData();
    rs::core::TrackView view{TrackId{0}, std::as_bytes(std::span{data}), std::nullopt, nullptr};

    auto prop = view.property();
    auto meta = view.metadata();

    CHECK(meta.artistId() == DictionaryId{1});
    CHECK(meta.albumId() == DictionaryId{2});
    CHECK(meta.genreId() == DictionaryId{3});
    CHECK(meta.albumArtistId() == DictionaryId{0});
    CHECK(meta.year() == 2020);
    CHECK(prop.codecId() == 0);
    CHECK(prop.bitDepth() == 16);
    CHECK(prop.rating() == 3);
    CHECK(view.tags().count() == 0);
  }

  TEST_CASE("TrackView (Hot) - String Accessors")
  {
    auto data = createTrackWithStrings("Test Title");
    rs::core::TrackView view{TrackId{0}, std::as_bytes(std::span{data}), std::nullopt, nullptr};

    CHECK(view.isHotValid() == true);
    CHECK(view.metadata().title() == "Test Title");
  }

  TEST_CASE("TrackView (Hot) - Empty String Handling")
  {
    auto data = createMinimalHotData();
    rs::core::TrackView view{TrackId{0}, std::as_bytes(std::span{data}), std::nullopt, nullptr};

    // Empty strings should return empty string_view
    CHECK(view.metadata().title().empty());
  }

  TEST_CASE("TrackView (Hot) - Invalid Data")
  {
    // Null data (valid size but nullptr) - isHotValid is false
    std::span<std::byte const> nullSpan{static_cast<std::byte const*>(nullptr), 100};
    rs::core::TrackView nullView{TrackId{0}, nullSpan, std::nullopt, nullptr};
    CHECK(nullView.isHotValid() == false);

    // Too small - isHotValid is false (no exception thrown)
    char smallData[10] = {};
    std::span<std::byte const> smallSpan{reinterpret_cast<std::byte const*>(smallData), sizeof(smallData)};
    rs::core::TrackView smallView{TrackId{0}, smallSpan, std::nullopt, nullptr};
    CHECK(smallView.isHotValid() == false);

    // Empty hot - isHotValid is false
    rs::core::TrackView emptyView{TrackId{0}, std::span<std::byte const>{}, std::nullopt, nullptr};
    CHECK(emptyView.isHotValid() == false);
  }

  TEST_CASE("TrackView (Hot) - Tag Bloom")
  {
    TrackHotHeader h{};
    h.tagBloom = 0xCAFE;

    auto data = serializeHeader(h);

    rs::core::TrackView view{TrackId{0}, std::as_bytes(std::span{data}), std::nullopt, nullptr};
    CHECK(view.tags().bloom() == 0xCAFE);
  }

  TEST_CASE("TrackView (Hot) - Tag Accessors - No Tags")
  {
    auto data = createTrackWithStrings("Test");
    rs::core::TrackView view{TrackId{0}, std::as_bytes(std::span{data}), std::nullopt, nullptr};

    CHECK(view.tags().count() == 0);
    CHECK(view.tags().id(0) == DictionaryId{0});

    auto ids = view.tags().ids();
    CHECK(ids.empty());
    CHECK(view.tags().has(DictionaryId{1}) == false);
  }

  TEST_CASE("TrackView (Hot) - Tag Accessors - With Tags")
  {
    // Create a track with 2 tags (tag IDs: 10, 20)
    TrackHotHeader h{};
    h.tagBloom = 0;
    h.artistId = DictionaryId{1};
    h.albumId = DictionaryId{2};
    h.genreId = DictionaryId{3};
    h.albumArtistId = DictionaryId{0};
    h.year = 2020;
    h.codecId = 0;
    h.bitDepth = 16;
    h.rating = 3;
    h.tagCount = 2;

    // Title at offset 0, tags after title
    std::string title = "Test Title";
    h.titleOffset = 0;
    h.titleLen = static_cast<std::uint16_t>(title.size());
    h.tagsOffset = static_cast<std::uint16_t>(title.size() + 1);

    auto data = serializeHeader(h);

    // Add title + null
    appendString(data, title);

    // Add tag IDs (4 bytes each)
    std::uint32_t tag1 = 10;
    std::uint32_t tag2 = 20;
    data.insert(data.end(), reinterpret_cast<std::byte const*>(&tag1), reinterpret_cast<std::byte const*>(&tag1 + 1));
    data.insert(data.end(), reinterpret_cast<std::byte const*>(&tag2), reinterpret_cast<std::byte const*>(&tag2 + 1));

    rs::core::TrackView view{TrackId{0}, std::as_bytes(std::span{data}), std::nullopt, nullptr};

    CHECK(view.tags().count() == 2);
    CHECK(view.tags().id(0) == DictionaryId{10});
    CHECK(view.tags().id(1) == DictionaryId{20});
    CHECK(view.tags().id(2) == DictionaryId{0}); // Out of bounds

    auto ids = view.tags().ids();
    CHECK(ids.size() == 2);
    CHECK(ids[0] == DictionaryId{10});
    CHECK(ids[1] == DictionaryId{20});

    CHECK(view.tags().has(DictionaryId{10}) == true);
    CHECK(view.tags().has(DictionaryId{20}) == true);
    CHECK(view.tags().has(DictionaryId{30}) == false);
  }

  // === Cold Layout Tests ===

  using rs::core::encodeColdData;
  using rs::core::normalizeKey;
  using rs::core::TrackColdHeader;

  // Helper to create a full cold data blob for testing
  std::vector<std::byte> createColdData(TrackColdHeader const& header = {},
                                        std::vector<std::pair<std::string, std::string>> const& customMeta = {},
                                        std::string_view uri = "")
  {
    return encodeColdData(header, customMeta, uri);
  }

  rs::core::TrackView makeColdView(std::vector<std::byte> const& data)
  {
    return rs::core::TrackView{TrackId{0}, std::span<std::byte const>{}, data, nullptr};
  }

  TEST_CASE("TrackColdHeader - Size and Alignment")
  {
    CHECK(sizeof(TrackColdHeader) == 48);
    CHECK(alignof(TrackColdHeader) == 4);
  }

  TEST_CASE("TrackView (Cold) - Cold-Only View")
  {
    // Create a cold-only view (hot data is empty, but cold data is present)
    auto data = createColdData();
    rs::core::TrackView view{TrackId{0}, std::span<std::byte const>{}, data, nullptr};
    CHECK(view.isHotValid() == false);
    CHECK(view.hasCold() == true);
    CHECK(view.isColdLoaded() == true);
  }

  TEST_CASE("TrackView (Cold) - Construct from Cold Data")
  {
    auto data = createColdData();
    rs::core::TrackView view{TrackId{0}, std::span<std::byte const>{}, data, nullptr};

    CHECK(view.hasCold() == true);
    CHECK(view.isColdLoaded() == true);
    CHECK(view.coldHeader() != nullptr);
  }

  TEST_CASE("TrackView (Cold) - Roundtrip Empty")
  {
    auto data = createColdData();
    auto view = makeColdView(data);

    CHECK(view.hasCold() == true);
    CHECK(view.isColdLoaded() == true);
    CHECK(view.custom().all().empty());
    CHECK(view.property().uri().empty());
  }

  TEST_CASE("TrackView (Cold) - Roundtrip Single Pair")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"key1", "value1"}};
    auto data = createColdData({}, pairs, "/path/to/file.flac");
    auto view = makeColdView(data);

    auto meta = view.custom().all();
    CHECK(meta.size() == 1);
    CHECK(meta[0].first == "key1");
    CHECK(meta[0].second == "value1");
    CHECK(view.property().uri() == "/path/to/file.flac");
  }

  TEST_CASE("TrackView (Cold) - Roundtrip Multiple Pairs")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{
      {"replaygain_track_gain_db", "-6.5"}, {"isrc", "USSM19999999"}, {"edition", "remaster"}};
    auto data = createColdData({}, pairs, "/path/to/file.flac");
    auto view = makeColdView(data);

    auto meta = view.custom().all();
    CHECK(meta.size() == 3);
    CHECK(meta[0].first == "replaygain_track_gain_db");
    CHECK(meta[0].second == "-6.5");
    CHECK(meta[1].first == "isrc");
    CHECK(meta[1].second == "USSM19999999");
    CHECK(meta[2].first == "edition");
    CHECK(meta[2].second == "remaster");
  }

  TEST_CASE("TrackView (Cold) - Custom Value Lookup - Found")
  {
    auto pairs =
      std::vector<std::pair<std::string, std::string>>{{"replaygain_track_gain_db", "-6.5"}, {"isrc", "USSM19999999"}};
    auto data = createColdData({}, pairs);
    auto view = makeColdView(data);

    auto value = view.custom().get("isrc");
    CHECK(value.has_value() == true);
    CHECK(*value == "USSM19999999");
  }

  TEST_CASE("TrackView (Cold) - Custom Value Lookup - Not Found")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"replaygain_track_gain_db", "-6.5"}};
    auto data = createColdData({}, pairs);
    auto view = makeColdView(data);

    auto value = view.custom().get("nonexistent");
    CHECK(value.has_value() == false);
  }

  TEST_CASE("TrackView (Cold) - Custom Value Lookup - Case Sensitive")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"ISRC", "USSM19999999"}};
    auto data = createColdData({}, pairs);
    auto view = makeColdView(data);

    auto value = view.custom().get("isrc");
    CHECK(value.has_value() == false);
  }

  TEST_CASE("TrackView (Cold) - Empty Key And Value")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"", ""}};
    auto data = createColdData({}, pairs);
    auto view = makeColdView(data);

    auto meta = view.custom().all();
    CHECK(meta.size() == 1);
    CHECK(meta[0].first.empty());
    CHECK(meta[0].second.empty());

    auto value = view.custom().get("");
    CHECK(value.has_value() == true);
    CHECK(value->empty());
  }

  TEST_CASE("TrackView (Cold) - Special Characters In Value")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"comment", "Hello, World! 你好"}};
    auto data = createColdData({}, pairs);
    auto view = makeColdView(data);

    auto meta = view.custom().all();
    CHECK(meta[0].second == "Hello, World! 你好");
  }

  TEST_CASE("TrackView (Cold) - Fixed Fields")
  {
    TrackColdHeader header{};
    std::tie(header.fileSizeLo, header.fileSizeHi) = splitInt64(12345678);
    std::tie(header.mtimeLo, header.mtimeHi) = splitInt64(987654321);
    header.coverArtId = 42;
    header.trackNumber = 5;
    header.totalTracks = 10;
    header.discNumber = 1;
    header.totalDiscs = 2;

    auto data = createColdData(header, {}, "/path/to/file.flac");
    auto view = makeColdView(data);

    CHECK(view.isColdLoaded() == true);
    auto prop = view.property();
    auto meta = view.metadata();
    CHECK(prop.fileSize() == 12345678);
    CHECK(prop.mtime() == 987654321);
    CHECK(meta.coverArtId() == 42);
    CHECK(meta.trackNumber() == 5);
    CHECK(meta.totalTracks() == 10);
    CHECK(meta.discNumber() == 1);
    CHECK(meta.totalDiscs() == 2);
    CHECK(prop.uri() == "/path/to/file.flac");
  }

  TEST_CASE("normalizeKey - Lowercase")
  {
    CHECK(normalizeKey("ISRC") == "isrc");
    CHECK(normalizeKey("ReplayGain_Track_Gain_DB") == "replaygain_track_gain_db");
    CHECK(normalizeKey("MiXeD_CaSe") == "mixed_case");
  }

  TEST_CASE("normalizeKey - Trim Whitespace")
  {
    CHECK(normalizeKey("  key") == "key");
    CHECK(normalizeKey("key  ") == "key");
    CHECK(normalizeKey("  key  ") == "key");
    CHECK(normalizeKey("\tkey\t") == "key");
  }

  TEST_CASE("normalizeKey - Lowercase And Trim")
  {
    CHECK(normalizeKey("  ISRC  ") == "isrc");
    CHECK(normalizeKey("\t ReplayGain \n") == "replaygain");
  }

  TEST_CASE("normalizeKey - Empty")
  {
    CHECK(normalizeKey("") == "");
    CHECK(normalizeKey("   ") == "");
  }

  TEST_CASE("normalizeKey - Special Characters Preserved")
  {
    CHECK(normalizeKey("replaygain_track_gain_db") == "replaygain_track_gain_db");
    CHECK(normalizeKey("isrc-code-123") == "isrc-code-123");
  }

} // anonymous namespace