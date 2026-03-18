// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <span>
#include <rs/core/TrackLayout.h>
#include <rs/core/TrackRecord.h>
#include <test/core/TestUtils.h>

#include <vector>

namespace
{
  using namespace test;
  using rs::core::DictionaryId;
  using rs::core::TrackHeader;
  using rs::core::TrackView;

  // Helper to create a minimal valid TrackView for testing
  std::vector<char> createMinimalData()
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

  std::vector<char> createTrackWithStrings(std::string_view title, std::string_view uri)
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
    TrackHeader h{};

    // Check 8-byte section
    CHECK(reinterpret_cast<char*>(&h.fileSize) - reinterpret_cast<char*>(&h) == 0);
    CHECK(reinterpret_cast<char*>(&h.mtime) - reinterpret_cast<char*>(&h) == 8);

    // Check 4-byte section starts at offset 16
    CHECK(reinterpret_cast<char*>(&h.tagBloom) - reinterpret_cast<char*>(&h) == 16);
    CHECK(reinterpret_cast<char*>(&h.durationMs) - reinterpret_cast<char*>(&h) == 20);

    // Check 2-byte section starts at offset 52
    CHECK(reinterpret_cast<char*>(&h.year) - reinterpret_cast<char*>(&h) == 52);
    CHECK(reinterpret_cast<char*>(&h.trackNumber) - reinterpret_cast<char*>(&h) == 54);
    CHECK(reinterpret_cast<char*>(&h.totalTracks) - reinterpret_cast<char*>(&h) == 56);
    CHECK(reinterpret_cast<char*>(&h.discNumber) - reinterpret_cast<char*>(&h) == 58);
    CHECK(reinterpret_cast<char*>(&h.totalDiscs) - reinterpret_cast<char*>(&h) == 60);

    // Check 1-byte section
    CHECK(reinterpret_cast<char*>(&h.channels) - reinterpret_cast<char*>(&h) == 74);
    CHECK(reinterpret_cast<char*>(&h.bitDepth) - reinterpret_cast<char*>(&h) == 75);
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
    // Null data
    std::span<std::byte const> nullSpan{static_cast<std::byte const*>(nullptr), 100};
    TrackView nullView(nullSpan);
    CHECK(nullView.isValid() == false);

    // Too small
    char smallData[10] = {};
    std::span<std::byte const> smallSpan{reinterpret_cast<std::byte const*>(smallData), sizeof(smallData)};
    TrackView smallView(smallSpan);
    CHECK(smallView.isValid() == false);

    // Empty
    TrackView emptyView;
    CHECK(emptyView.isValid() == false);
  }

  TEST_CASE("TrackView - Payload Access")
  {
    auto data = createTrackWithStrings("Hello", "/world");
    TrackView view(std::as_bytes(std::span{data}));

    auto payload = view.payload();
    CHECK(payload.size() > 0);
    // Payload should contain "Hello\0/world\0"
    CHECK(payload.find("Hello") != std::string_view::npos);
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
    data.insert(data.end(), reinterpret_cast<char const*>(&tag1), reinterpret_cast<char const*>(&tag1 + 1));
    data.insert(data.end(), reinterpret_cast<char const*>(&tag2), reinterpret_cast<char const*>(&tag2 + 1));

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

} // anonymous namespace
