// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <cstddef>
#include <rs/core/TrackView.h>
#include <rs/core/TrackRecord.h>
#include <span>
#include <test/core/TestUtils.h>

#include <vector>

namespace
{
  using namespace test;
  using rs::core::DictionaryId;
  using rs::core::TrackHeader;
  using rs::core::TrackView;

  // Helper to create a minimal valid TrackView for testing
  std::vector<std::byte> createMinimalData()
  {
    TrackHeader h{};
    h.fileSize = 1000;
    h.mtime = 1234567890;
    h.durationMs = 180000;
    h.bitrate = 320000;
    h.sampleRate = 44100;
    h.artistId = DictionaryId{1};
    h.albumId = DictionaryId{2};
    h.genreId = DictionaryId{3};
    h.albumArtistId = DictionaryId{0};
    h.year = 2020;
    h.trackNumber = 5;
    h.codecId = 0;
    h.channels = 2;
    h.bitDepth = 16;
    h.rating = 3;
    h.tagCount = 0;
    h.titleOffset = 0;
    h.titleLen = 0;
    h.uriOffset = 0;
    h.uriLen = 0;
    h.tagsOffset = 0;

    return serializeHeader(h);
  }

  std::vector<std::byte> createTrackWithStrings(std::string_view title, std::string_view uri)
  {
    TrackHeader h{};
    h.fileSize = 1000;
    h.mtime = 1234567890;
    h.durationMs = 180000;
    h.bitrate = 320000;
    h.sampleRate = 44100;
    h.artistId = DictionaryId{1};
    h.albumId = DictionaryId{2};
    h.genreId = DictionaryId{3};
    h.albumArtistId = DictionaryId{0};
    h.year = 2020;
    h.trackNumber = 5;
    h.codecId = 0;
    h.channels = 2;
    h.bitDepth = 16;
    h.rating = 3;
    h.tagCount = 0;

    // Title at offset 0 in payload, URI after title + null terminator
    h.titleOffset = 0;
    h.titleLen = static_cast<std::uint16_t>(title.size());
    h.uriOffset = static_cast<std::uint16_t>(title.size() + 1);
    h.uriLen = static_cast<std::uint16_t>(uri.size());
    h.tagsOffset = static_cast<std::uint16_t>(title.size() + 1 + uri.size() + 1);

    auto data = serializeHeader(h);

    // Add title + null + uri + null
    appendString(data, title);
    appendString(data, uri);

    return data;
  }

  TEST_CASE("TrackHeader - Size and Alignment")
  {
    CHECK(sizeof(TrackHeader) == 80);
    CHECK(alignof(TrackHeader) == 8);
  }

  TEST_CASE("TrackHeader - Field Offsets")
  {
    // Verify key field offsets for ABI compatibility
    // Check 8-byte section
    CHECK(offsetof(TrackHeader, fileSize) == 0);
    CHECK(offsetof(TrackHeader, mtime) == 8);

    // Check 4-byte section starts at offset 16
    CHECK(offsetof(TrackHeader, tagBloom) == 16);
    CHECK(offsetof(TrackHeader, durationMs) == 20);

    // Check 2-byte section starts at offset 52
    CHECK(offsetof(TrackHeader, year) == 52);
    CHECK(offsetof(TrackHeader, trackNumber) == 54);
    CHECK(offsetof(TrackHeader, totalTracks) == 56);
    CHECK(offsetof(TrackHeader, discNumber) == 58);
    CHECK(offsetof(TrackHeader, totalDiscs) == 60);

    // Check 1-byte section
    CHECK(offsetof(TrackHeader, channels) == 74);
    CHECK(offsetof(TrackHeader, bitDepth) == 75);
  }

  TEST_CASE("TrackView - Default Constructor")
  {
    TrackView view;
    CHECK(view.isValid() == false);
  }

  TEST_CASE("TrackView - Construct from Data")
  {
    auto data = createMinimalData();
    TrackView view(std::as_bytes(std::span{data}));

    CHECK(view.isValid() == true);
    CHECK(view.header() != nullptr);
  }

  TEST_CASE("TrackView - Fixed Field Accessors")
  {
    auto data = createMinimalData();
    TrackView view(std::as_bytes(std::span{data}));

    auto prop = view.property();
    auto meta = view.metadata();

    CHECK(prop.fileSize() == 1000);
    CHECK(prop.mtime() == 1234567890);
    CHECK(prop.durationMs() == 180000);
    CHECK(prop.bitrate() == 320000);
    CHECK(prop.sampleRate() == 44100);
    CHECK(meta.artistId() == 1);
    CHECK(meta.albumId() == 2);
    CHECK(meta.genreId() == 3);
    CHECK(meta.albumArtistId() == 0);
    CHECK(meta.year() == 2020);
    CHECK(meta.trackNumber() == 5);
    CHECK(prop.codecId() == 0);
    CHECK(prop.channels() == 2);
    CHECK(prop.bitDepth() == 16);
    CHECK(prop.rating() == 3);
    CHECK(view.tags().count() == 0);
  }

  TEST_CASE("TrackView - String Accessors")
  {
    auto data = createTrackWithStrings("Test Title", "/path/to/file.flac");
    TrackView view(std::as_bytes(std::span{data}));

    CHECK(view.isValid() == true);
    CHECK(view.metadata().title() == "Test Title");
    CHECK(view.property().uri() == "/path/to/file.flac");
  }

  TEST_CASE("TrackView - Empty String Handling")
  {
    auto data = createMinimalData();
    TrackView view(std::as_bytes(std::span{data}));

    // Empty strings should return empty string_view
    CHECK(view.metadata().title().empty());
    CHECK(view.property().uri().empty());
  }

  TEST_CASE("TrackView - Invalid Data")
  {
    // Null data (valid size but nullptr) - creates invalid view, isValid is false
    std::span<std::byte const> nullSpan{static_cast<std::byte const*>(nullptr), 100};
    TrackView nullView(nullSpan);
    CHECK(nullView.isValid() == false);

    // Too small - throws on construction
    char smallData[10] = {};
    std::span<std::byte const> smallSpan{reinterpret_cast<std::byte const*>(smallData), sizeof(smallData)};
    CHECK_THROWS(TrackView(smallSpan));

    // Empty - uses default constructor, isValid is false
    TrackView emptyView;
    CHECK(emptyView.isValid() == false);
  }

  TEST_CASE("TrackView - Tag Bloom")
  {
    TrackHeader h{};
    h.tagBloom = 0xCAFE;

    auto data = serializeHeader(h);

    TrackView view(std::as_bytes(std::span{data}));
    CHECK(view.tags().bloom() == 0xCAFE);
  }

  TEST_CASE("TrackView - Tag Accessors - No Tags")
  {
    auto data = createTrackWithStrings("Test", "/path/to/file.flac");
    TrackView view(std::as_bytes(std::span{data}));

    CHECK(view.tags().count() == 0);
    CHECK(view.tags().id(0) == 0);

    auto ids = view.tags().ids();
    CHECK(ids.empty());
    CHECK(view.tags().has(DictionaryId{1}) == false);
  }

  TEST_CASE("TrackView - Tag Accessors - With Tags")
  {
    // Create a track with 2 tags (tag IDs: 10, 20)
    TrackHeader h{};
    h.fileSize = 1000;
    h.mtime = 1234567890;
    h.durationMs = 180000;
    h.bitrate = 320000;
    h.sampleRate = 44100;
    h.artistId = DictionaryId{1};
    h.albumId = DictionaryId{2};
    h.genreId = DictionaryId{3};
    h.albumArtistId = DictionaryId{0};
    h.year = 2020;
    h.trackNumber = 5;
    h.codecId = 0;
    h.channels = 2;
    h.bitDepth = 16;
    h.rating = 3;
    h.tagCount = 2;

    // Title at offset 0, URI after title, tags after URI
    std::string title = "Test Title";
    std::string uri = "/path/to/file.flac";
    h.titleOffset = 0;
    h.titleLen = static_cast<std::uint16_t>(title.size());
    h.uriOffset = static_cast<std::uint16_t>(title.size() + 1);
    h.uriLen = static_cast<std::uint16_t>(uri.size());
    h.tagsOffset = static_cast<std::uint16_t>(title.size() + 1 + uri.size() + 1);

    auto data = serializeHeader(h);

    // Add title + null + uri + null
    appendString(data, title);
    appendString(data, uri);

    // Add tag IDs (4 bytes each)
    std::uint32_t tag1 = 10;
    std::uint32_t tag2 = 20;
    data.insert(data.end(), reinterpret_cast<std::byte const*>(&tag1), reinterpret_cast<std::byte const*>(&tag1 + 1));
    data.insert(data.end(), reinterpret_cast<std::byte const*>(&tag2), reinterpret_cast<std::byte const*>(&tag2 + 1));

    TrackView view(std::as_bytes(std::span{data}));

    CHECK(view.tags().count() == 2);
    CHECK(view.tags().id(0) == 10);
    CHECK(view.tags().id(1) == 20);
    CHECK(view.tags().id(2) == 0); // Out of bounds

    auto ids = view.tags().ids();
    CHECK(ids.size() == 2);
    CHECK(ids[0] == 10);
    CHECK(ids[1] == 20);

    CHECK(view.tags().has(DictionaryId{10}) == true);
    CHECK(view.tags().has(DictionaryId{20}) == true);
    CHECK(view.tags().has(DictionaryId{30}) == false);
  }

  // === Cold Layout Tests ===

  using rs::core::encodeColdData;
  using rs::core::normalizeKey;
  using rs::core::TrackColdHeader;
  using rs::core::TrackColdView;

  // Helper to create a full cold data blob for testing
  std::vector<std::byte> createColdData(
      TrackColdHeader const& header = {},
      std::vector<std::pair<std::string, std::string>> const& customMeta = {},
      std::string_view uri = "")
  {
    return encodeColdData(header, customMeta, uri);
  }

  TrackColdView makeColdView(std::vector<std::byte> const& data)
  {
    return TrackColdView(std::as_bytes(std::span{data}));
  }

  TEST_CASE("TrackColdHeader - Size and Alignment")
  {
    CHECK(sizeof(TrackColdHeader) == 40);
    CHECK(alignof(TrackColdHeader) == 8);
  }

  TEST_CASE("TrackColdView - Default Constructor")
  {
    TrackColdView view;
    CHECK(view.isNull() == true);
    CHECK(view.isEmpty() == true);
    CHECK(view.isValid() == false);
    CHECK(view.size() == 0);
  }

  TEST_CASE("TrackColdView - Empty Data - Default Constructor")
  {
    // Empty data uses default constructor (not span constructor which throws)
    TrackColdView view;
    CHECK(view.isNull() == true);
    CHECK(view.isEmpty() == true);
    CHECK(view.isValid() == false);
  }

  TEST_CASE("TrackColdView - Empty Span - Throws")
  {
    std::vector<std::byte> empty;
    std::span<std::byte const> emptySpan{empty};
    CHECK_THROWS(TrackColdView(emptySpan));
  }

  TEST_CASE("TrackColdView - Invalid Data - Throws")
  {
    char smallData[10] = {};
    std::span<std::byte const> smallSpan{reinterpret_cast<std::byte const*>(smallData), sizeof(smallData)};
    CHECK_THROWS(TrackColdView(smallSpan));
  }

  TEST_CASE("TrackColdView - Roundtrip Empty")
  {
    auto data = createColdData();
    auto view = makeColdView(data);

    CHECK(view.isNull() == false);
    CHECK(view.isEmpty() == false);
    CHECK(view.isValid() == true);
    CHECK(view.customMeta().empty());
    CHECK(view.uri().empty());
  }

  TEST_CASE("TrackColdView - Roundtrip Single Pair")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"key1", "value1"}};
    auto data = createColdData({}, pairs, "/path/to/file.flac");
    auto view = makeColdView(data);

    auto meta = view.customMeta();
    CHECK(meta.size() == 1);
    CHECK(meta[0].first == "key1");
    CHECK(meta[0].second == "value1");
    CHECK(view.uri() == "/path/to/file.flac");
  }

  TEST_CASE("TrackColdView - Roundtrip Multiple Pairs")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{
      {"replaygain_track_gain_db", "-6.5"},
      {"isrc", "USSM19999999"},
      {"edition", "remaster"}
    };
    auto data = createColdData({}, pairs, "/path/to/file.flac");
    auto view = makeColdView(data);

    auto meta = view.customMeta();
    CHECK(meta.size() == 3);
    CHECK(meta[0].first == "replaygain_track_gain_db");
    CHECK(meta[0].second == "-6.5");
    CHECK(meta[1].first == "isrc");
    CHECK(meta[1].second == "USSM19999999");
    CHECK(meta[2].first == "edition");
    CHECK(meta[2].second == "remaster");
  }

  TEST_CASE("TrackColdView - Custom Value Lookup - Found")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{
      {"replaygain_track_gain_db", "-6.5"},
      {"isrc", "USSM19999999"}
    };
    auto data = createColdData({}, pairs);
    auto view = makeColdView(data);

    auto value = view.customValue("isrc");
    CHECK(value.has_value() == true);
    CHECK(*value == "USSM19999999");
  }

  TEST_CASE("TrackColdView - Custom Value Lookup - Not Found")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{
      {"replaygain_track_gain_db", "-6.5"}
    };
    auto data = createColdData({}, pairs);
    auto view = makeColdView(data);

    auto value = view.customValue("nonexistent");
    CHECK(value.has_value() == false);
  }

  TEST_CASE("TrackColdView - Custom Value Lookup - Case Sensitive")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{
      {"ISRC", "USSM19999999"}
    };
    auto data = createColdData({}, pairs);
    auto view = makeColdView(data);

    auto value = view.customValue("isrc");
    CHECK(value.has_value() == false);
  }

  TEST_CASE("TrackColdView - Empty Key And Value")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"", ""}};
    auto data = createColdData({}, pairs);
    auto view = makeColdView(data);

    auto meta = view.customMeta();
    CHECK(meta.size() == 1);
    CHECK(meta[0].first.empty());
    CHECK(meta[0].second.empty());

    auto value = view.customValue("");
    CHECK(value.has_value() == true);
    CHECK(value->empty());
  }

  TEST_CASE("TrackColdView - Special Characters In Value")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{
      {"comment", "Hello, World! 你好"}
    };
    auto data = createColdData({}, pairs);
    auto view = makeColdView(data);

    auto meta = view.customMeta();
    CHECK(meta[0].second == "Hello, World! 你好");
  }

  TEST_CASE("TrackColdView - Fixed Fields")
  {
    TrackColdHeader header{};
    header.fileSize = 12345678;
    header.mtime = 987654321;
    header.coverArtId = 42;
    header.trackNumber = 5;
    header.totalTracks = 10;
    header.discNumber = 1;
    header.totalDiscs = 2;

    auto data = createColdData(header, {}, "/path/to/file.flac");
    auto view = makeColdView(data);

    CHECK(view.isValid() == true);
    CHECK(view.fileSize() == 12345678);
    CHECK(view.mtime() == 987654321);
    CHECK(view.coverArtId() == 42);
    CHECK(view.trackNumber() == 5);
    CHECK(view.totalTracks() == 10);
    CHECK(view.discNumber() == 1);
    CHECK(view.totalDiscs() == 2);
    CHECK(view.uri() == "/path/to/file.flac");
  }

  TEST_CASE("TrackColdView - PropertyProxy")
  {
    TrackColdHeader header{};
    header.fileSize = 12345678;
    header.mtime = 987654321;
    header.coverArtId = 42;
    header.trackNumber = 5;
    header.totalTracks = 10;
    header.discNumber = 1;
    header.totalDiscs = 2;

    auto data = createColdData(header, {}, "/path/to/file.flac");
    auto view = makeColdView(data);

    auto prop = view.property();
    CHECK(prop.fileSize() == 12345678);
    CHECK(prop.mtime() == 987654321);
    CHECK(prop.coverArtId() == 42);
    CHECK(prop.trackNumber() == 5);
    CHECK(prop.totalTracks() == 10);
    CHECK(prop.discNumber() == 1);
    CHECK(prop.totalDiscs() == 2);
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
