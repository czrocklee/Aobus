// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <cstddef>
#include <rs/core/TrackRecord.h>
#include <rs/core/TrackView.h>
#include <rs/utility/ByteView.h>
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
    h.tagLen = 0;
    h.titleLen = 0;

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
    h.tagLen = 0;  // no tags

    // In new layout: tags first (at sizeof(TrackHotHeader)), title after (at sizeof(TrackHotHeader) + tagLen)
    h.titleLen = static_cast<std::uint16_t>(title.size());

    auto data = serializeHeader(h);

    // Add title + null
    appendString(data, title);

    return data;
  }

  TEST_CASE("TrackHotHeader - Size and Alignment")
  {
    CHECK(sizeof(TrackHotHeader) == 32);
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
    CHECK(offsetof(TrackHotHeader, titleLen) == 26);
    CHECK(offsetof(TrackHotHeader, tagLen) == 28);

    // Check 1-byte section
    CHECK(offsetof(TrackHotHeader, rating) == 30);
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

    std::vector<DictionaryId> ids(view.tags().begin(), view.tags().end());
    CHECK(ids.empty());
    CHECK(view.tags().has(DictionaryId{1}) == false);
  }

  TEST_CASE("TrackView (Hot) - Tag Accessors - With Tags")
  {
    // Create a track with 2 tags (tag IDs: 10, 20)
    // In new layout: tags first, then title
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
    h.tagLen = 8;  // 2 tags * 4 bytes

    std::string title = "Test Title";
    h.titleLen = static_cast<std::uint16_t>(title.size());

    auto data = serializeHeader(h);

    // Add tag IDs first (at sizeof(TrackHotHeader))
    std::uint32_t tag1 = 10;
    std::uint32_t tag2 = 20;
    data.insert(data.end(), reinterpret_cast<std::byte const*>(&tag1), reinterpret_cast<std::byte const*>(&tag1 + 1));
    data.insert(data.end(), reinterpret_cast<std::byte const*>(&tag2), reinterpret_cast<std::byte const*>(&tag2 + 1));

    // Add title + null (after tags)
    appendString(data, title);

    rs::core::TrackView view{TrackId{0}, std::as_bytes(std::span{data}), std::nullopt, nullptr};

    CHECK(view.tags().count() == 2);
    CHECK(view.tags().id(0) == DictionaryId{10});
    CHECK(view.tags().id(1) == DictionaryId{20});
    CHECK(view.tags().id(2) == DictionaryId{0}); // Out of bounds

    std::vector<DictionaryId> ids(view.tags().begin(), view.tags().end());
    CHECK(ids.size() == 2);
    CHECK(ids[0] == DictionaryId{10});
    CHECK(ids[1] == DictionaryId{20});

    CHECK(view.tags().has(DictionaryId{10}) == true);
    CHECK(view.tags().has(DictionaryId{20}) == true);
    CHECK(view.tags().has(DictionaryId{30}) == false);
  }

  // === Cold Layout Tests ===

  using rs::core::TrackColdHeader;

  // Helper to create a full cold data blob for testing using TrackRecord
  std::vector<std::byte> createColdData(TrackColdHeader const& header = {},
                                        std::vector<std::pair<std::string, std::string>> const& customMeta = {},
                                        std::string_view uri = "")
  {
    rs::core::TrackRecord record;
    record.cold.uri = std::string{uri};
    record.cold.fileSize = rs::utility::combineInt64(header.fileSizeLo, header.fileSizeHi);
    record.cold.mtime = rs::utility::combineInt64(header.mtimeLo, header.mtimeHi);
    record.cold.coverArtId = header.coverArtId;
    record.cold.trackNumber = header.trackNumber;
    record.cold.totalTracks = header.totalTracks;
    record.cold.discNumber = header.discNumber;
    record.cold.totalDiscs = header.totalDiscs;
    record.property.durationMs = header.durationMs;
    record.property.sampleRate = header.sampleRate;
    record.property.bitrate = header.bitrate;
    record.property.channels = header.channels;
    record.customMeta = customMeta;
    return record.serializeCold();
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
    int count = 0;
    for (auto const& [k, v] : view.custom()) { (void)k; (void)v; ++count; }
    CHECK(count == 0);
    CHECK(view.property().uri().empty());
  }

  TEST_CASE("TrackView (Cold) - Roundtrip Single Pair")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"key1", "value1"}};
    auto data = createColdData({}, pairs, "/path/to/file.flac");
    auto view = makeColdView(data);

    int count = 0;
    for (auto const& [k, v] : view.custom()) {
      CHECK(k == "key1");
      CHECK(v == "value1");
      ++count;
    }
    CHECK(count == 1);
    CHECK(view.property().uri() == "/path/to/file.flac");
  }

  TEST_CASE("TrackView (Cold) - Roundtrip Multiple Pairs")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{
      {"replaygain_track_gain_db", "-6.5"}, {"isrc", "USSM19999999"}, {"edition", "remaster"}};
    auto data = createColdData({}, pairs, "/path/to/file.flac");
    auto view = makeColdView(data);

    std::vector<std::pair<std::string, std::string>> result;
    for (auto const& [k, v] : view.custom()) {
      result.emplace_back(std::string{k}, std::string{v});
    }
    CHECK(result.size() == 3);
    CHECK(result[0].first == "replaygain_track_gain_db");
    CHECK(result[0].second == "-6.5");
    CHECK(result[1].first == "isrc");
    CHECK(result[1].second == "USSM19999999");
    CHECK(result[2].first == "edition");
    CHECK(result[2].second == "remaster");
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

    int count = 0;
    for (auto const& [k, v] : view.custom()) {
      CHECK(k.empty());
      CHECK(v.empty());
      ++count;
    }
    CHECK(count == 1);

    auto value = view.custom().get("");
    CHECK(value.has_value() == true);
    CHECK(value->empty());
  }

  TEST_CASE("TrackView (Cold) - Special Characters In Value")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"comment", "Hello, World! 你好"}};
    auto data = createColdData({}, pairs);
    auto view = makeColdView(data);

    for (auto const& [k, v] : view.custom()) {
      CHECK(k == "comment");
      CHECK(v == "Hello, World! 你好");
    }
  }

  TEST_CASE("TrackView (Cold) - CustomProxy Iterator Empty")
  {
    auto data = createColdData({}, {}, "");
    auto view = makeColdView(data);

    int count = 0;
    for (auto [key, value] : view.custom()) {
      (void)key; (void)value;
      ++count;
    }
    CHECK(count == 0);
  }

  TEST_CASE("TrackView (Cold) - CustomProxy Iterator Single Pair")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"key1", "value1"}};
    auto data = createColdData({}, pairs);
    auto view = makeColdView(data);

    int count = 0;
    for (auto [key, value] : view.custom()) {
      CHECK(key == "key1");
      CHECK(value == "value1");
      ++count;
    }
    CHECK(count == 1);
  }

  TEST_CASE("TrackView (Cold) - CustomProxy Iterator Multiple Pairs")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{
      {"replaygain_track_gain_db", "-6.5"},
      {"isrc", "USSM19999999"},
      {"edition", "remaster"}
    };
    auto data = createColdData({}, pairs);
    auto view = makeColdView(data);

    std::vector<std::pair<std::string, std::string>> result;
    for (auto [key, value] : view.custom()) {
      result.emplace_back(std::string{key}, std::string{value});
    }
    CHECK(result.size() == 3);
    CHECK(result[0].first == "replaygain_track_gain_db");
    CHECK(result[0].second == "-6.5");
    CHECK(result[1].first == "isrc");
    CHECK(result[1].second == "USSM19999999");
    CHECK(result[2].first == "edition");
    CHECK(result[2].second == "remaster");
  }

  TEST_CASE("TrackView (Cold) - CustomProxy Iterator Special Characters")
  {
    auto pairs = std::vector<std::pair<std::string, std::string>>{{"comment", "Hello, World! 你好"}};
    auto data = createColdData({}, pairs);
    auto view = makeColdView(data);

    for (auto [key, value] : view.custom()) {
      CHECK(key == "comment");
      CHECK(value == "Hello, World! 你好");
    }
  }

  TEST_CASE("TrackView (Cold) - CustomProxy Iterator Truncated Length Header")
  {
    TrackColdHeader header{};
    header.customLen = 2;
    auto data = serializeHeader(header);
    data.push_back(std::byte{0x01});
    data.push_back(std::byte{0x00});

    auto view = makeColdView(data);

    int count = 0;
    for (auto const& [k, v] : view.custom()) {
      (void)k;
      (void)v;
      ++count;
    }
    CHECK(count == 0);
    CHECK(view.custom().get("any").has_value() == false);
  }

  TEST_CASE("TrackView (Cold) - CustomProxy Iterator Truncated Payload")
  {
    TrackColdHeader header{};
    header.customLen = 6;
    auto data = serializeHeader(header);

    std::uint16_t keyLen = 3;
    std::uint16_t valueLen = 2;
    data.insert(data.end(),
                reinterpret_cast<std::byte const*>(&keyLen),
                reinterpret_cast<std::byte const*>(&keyLen) + sizeof(keyLen));
    data.insert(data.end(),
                reinterpret_cast<std::byte const*>(&valueLen),
                reinterpret_cast<std::byte const*>(&valueLen) + sizeof(valueLen));
    data.push_back(std::byte{'a'});
    data.push_back(std::byte{'b'});

    auto view = makeColdView(data);

    int count = 0;
    for (auto const& [k, v] : view.custom()) {
      (void)k;
      (void)v;
      ++count;
    }
    CHECK(count == 0);
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

} // anonymous namespace
