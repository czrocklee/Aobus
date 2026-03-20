// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <rs/core/TrackLayout.h>
#include <rs/core/TrackRecord.h>

#include <cstring>

using rs::core::DictionaryId;
using rs::core::TrackColdHeader;
using rs::core::TrackHotHeader;
using rs::core::TrackId;
using rs::core::TrackRecord;
using rs::core::TrackView;

TEST_CASE("TrackRecord - Default Constructor")
{
  TrackRecord record;

  CHECK(record.metadata.title.empty());
  CHECK(record.metadata.uri.empty());
  CHECK(record.metadata.artist.empty());
  CHECK(record.metadata.album.empty());
  CHECK(record.metadata.albumArtist.empty());
  CHECK(record.metadata.genre.empty());
  CHECK(record.metadata.year == 0);
  CHECK(record.metadata.trackNumber == 0);
  CHECK(record.metadata.totalTracks == 0);
  CHECK(record.metadata.discNumber == 0);
  CHECK(record.metadata.totalDiscs == 0);
  CHECK(record.metadata.coverArtId == 0);
  CHECK(record.property.fileSize == 0);
  CHECK(record.property.mtime == 0);
  CHECK(record.property.durationMs == 0);
  CHECK(record.property.bitrate == 0);
  CHECK(record.property.sampleRate == 0);
  CHECK(record.property.codecId == 0);
  CHECK(record.property.channels == 0);
  CHECK(record.property.bitDepth == 0);
  CHECK(record.property.rating == 0);
  CHECK(record.tags.ids.empty());
}

TEST_CASE("TrackRecord - Field Assignment")
{
  TrackRecord record;
  record.metadata.title = "Test Title";
  record.metadata.uri = "/path/to/track.flac";
  record.metadata.artist = "Test Artist";
  record.metadata.album = "Test Album";
  record.metadata.albumArtist = "";
  record.metadata.genre = "Rock";
  record.metadata.year = 2020;
  record.metadata.trackNumber = 5;
  record.metadata.totalTracks = 10;
  record.metadata.discNumber = 2;
  record.metadata.totalDiscs = 3;
  record.metadata.coverArtId = 42;
  record.property.fileSize = 10000000;
  record.property.mtime = 1234567890;
  record.property.durationMs = 180000;
  record.property.bitrate = 320000;
  record.property.sampleRate = 44100;
  record.property.codecId = 7;
  record.property.channels = 2;
  record.property.bitDepth = 16;
  record.property.rating = 4;

  CHECK(record.metadata.title == "Test Title");
  CHECK(record.metadata.uri == "/path/to/track.flac");
  CHECK(record.metadata.artist == "Test Artist");
  CHECK(record.metadata.album == "Test Album");
  CHECK(record.metadata.albumArtist == "");
  CHECK(record.metadata.genre == "Rock");
  CHECK(record.metadata.year == 2020);
  CHECK(record.metadata.trackNumber == 5);
  CHECK(record.metadata.totalTracks == 10);
  CHECK(record.metadata.discNumber == 2);
  CHECK(record.metadata.totalDiscs == 3);
  CHECK(record.metadata.coverArtId == 42);
  CHECK(record.property.fileSize == 10000000);
  CHECK(record.property.mtime == 1234567890);
  CHECK(record.property.durationMs == 180000);
  CHECK(record.property.bitrate == 320000);
  CHECK(record.property.sampleRate == 44100);
  CHECK(record.property.codecId == 7);
  CHECK(record.property.channels == 2);
  CHECK(record.property.bitDepth == 16);
  CHECK(record.property.rating == 4);
}

TEST_CASE("TrackRecord - Serialize Empty Record")
{
  TrackRecord record;
  auto data = record.serializeHot();

  CHECK(data.size() >= sizeof(TrackHotHeader));
  CHECK(!data.empty());
}

TEST_CASE("TrackRecord - Serialize With Strings")
{
  TrackRecord record;
  record.metadata.title = "Hello World";
  record.metadata.uri = "/music/test.flac";
  record.metadata.year = 2021;

  auto data = record.serializeHot();

  // Verify header size
  CHECK(data.size() >= sizeof(TrackHotHeader));

  // Parse the serialized data back
  auto const* header = reinterpret_cast<TrackHotHeader const*>(data.data());

  CHECK(header->titleLen == 11); // "Hello World"
  CHECK(header->year == 2021);

  // Verify strings are in the payload
  auto payloadStart = reinterpret_cast<char const*>(data.data()) + sizeof(TrackHotHeader);
  CHECK(std::strncmp(payloadStart, "Hello World", 11) == 0);
}

TEST_CASE("TrackRecord - hotHeader Method")
{
  TrackRecord record;
  record.metadata.year = 1999;
  record.property.bitDepth = 24;
  record.property.rating = 5;

  auto header = record.hotHeader();

  CHECK(header.year == 1999);
  CHECK(header.bitDepth == 24);
  CHECK(header.rating == 5);
}

TEST_CASE("TrackRecord - Serialize With Special Characters")
{
  TrackRecord record;
  record.metadata.title = "Test: \"Quotes\" & 'Apostrophes'";

  auto data = record.serializeHot();

  auto const* header = reinterpret_cast<TrackHotHeader const*>(data.data());
  CHECK(header->titleLen == record.metadata.title.size());
}

TEST_CASE("TrackRecord - Serialize Preserves Data")
{
  TrackRecord record;
  record.metadata.title = "Test";
  record.metadata.uri = "/test";
  record.property.fileSize = 12345;
  record.property.mtime = 9876543210;

  auto data = record.serializeHot();
  auto data2 = record.serializeHot();

  // Multiple serializations should produce same size and content
  CHECK(data.size() == data2.size());
  CHECK(data == data2);
}

TEST_CASE("TrackRecord - Tag Serialization - Empty Tags")
{
  TrackRecord record;
  record.metadata.title = "Test";
  record.metadata.uri = "/test";

  auto data = record.serializeHot();

  auto const* header = reinterpret_cast<TrackHotHeader const*>(data.data());
  CHECK(header->tagLen == 0);
  CHECK(header->tagBloom == 0);
}

TEST_CASE("TrackRecord - Tag Serialization - With Tags")
{
  TrackRecord record;
  record.metadata.title = "Test";
  record.metadata.uri = "/test";
  record.tags.ids = {DictionaryId{10}, DictionaryId{20}, DictionaryId{30}};

  auto data = record.serializeHot();

  auto const* header = reinterpret_cast<TrackHotHeader const*>(data.data());
  CHECK(header->tagLen == 12);  // 3 tags * 4 bytes each
  CHECK(header->tagBloom != 0); // Bloom should be computed from tag IDs

  // Verify tag IDs are in the payload (at sizeof(TrackHotHeader))
  auto const* tagIdsPtr = reinterpret_cast<std::uint32_t const*>(data.data() + sizeof(TrackHotHeader));
  CHECK(tagIdsPtr[0] == 10);
  CHECK(tagIdsPtr[1] == 20);
  CHECK(tagIdsPtr[2] == 30);
}

TEST_CASE("TrackRecord - Tag Serialization - Single Tag")
{
  TrackRecord record;
  record.metadata.title = "Test";
  record.metadata.uri = "/test";
  record.tags.ids = {DictionaryId{42}};

  auto data = record.serializeHot();

  auto const* header = reinterpret_cast<TrackHotHeader const*>(data.data());
  CHECK(header->tagLen == 4);  // 1 tag * 4 bytes

  auto const* tagIdPtr = reinterpret_cast<std::uint32_t const*>(data.data() + sizeof(TrackHotHeader));
  CHECK(*tagIdPtr == 42);
}

TEST_CASE("TrackRecord - hotHeader Method With Tags")
{
  TrackRecord record;
  record.metadata.title = "Test";
  record.metadata.uri = "/test";
  record.tags.ids = {DictionaryId{1}, DictionaryId{2}, DictionaryId{3}, DictionaryId{4}, DictionaryId{5}};

  auto header = record.hotHeader();

  CHECK(header.tagLen == 20);  // 5 tags * 4 bytes each
  CHECK(header.tagBloom != 0);
}

TEST_CASE("TrackRecord - hotHeader")
{
  TrackRecord record;
  record.tags.ids = {DictionaryId{1}, DictionaryId{2}};

  auto header = record.hotHeader();

  CHECK(header.tagLen == 8);  // 2 tags * 4 bytes each
  CHECK(header.tagBloom != 0);
}

TEST_CASE("TrackRecord - coldHeader")
{
  TrackRecord record;
  record.property.fileSize = 2000;
  record.property.mtime = 1234567890;
  record.metadata.trackNumber = 5;
  record.metadata.totalTracks = 10;
  record.metadata.discNumber = 1;
  record.metadata.totalDiscs = 2;
  record.metadata.uri = "/path/to/file.flac";

  auto header = record.coldHeader();

  CHECK(header.fileSizeLo == static_cast<std::uint32_t>(2000 & 0xFFFFFFFF));
  CHECK(header.fileSizeHi == static_cast<std::uint32_t>(static_cast<std::uint64_t>(2000) >> 32));
  CHECK(header.mtimeLo == static_cast<std::uint32_t>(1234567890 & 0xFFFFFFFF));
  CHECK(header.mtimeHi == static_cast<std::uint32_t>(static_cast<std::uint64_t>(1234567890) >> 32));
  CHECK(header.trackNumber == 5);
  CHECK(header.totalTracks == 10);
  CHECK(header.discNumber == 1);
  CHECK(header.totalDiscs == 2);
}

TEST_CASE("TrackRecord - serializeHot")
{
  TrackRecord record;
  record.property.fileSize = 1000;
  record.metadata.title = "Test Title";
  record.metadata.uri = "/path/to/file.flac";
  record.tags.ids = {DictionaryId{10}, DictionaryId{20}};

  auto data = record.serializeHot();

  // Verify hot header
  auto const* header = reinterpret_cast<TrackHotHeader const*>(data.data());
  CHECK(header->tagLen == 8);  // 2 tags * 4 bytes

  // Verify bloom is computed
  CHECK(header->tagBloom != 0);
}

TEST_CASE("TrackRecord - serializeCold")
{
  TrackRecord record;
  record.property.fileSize = 2000;
  record.property.mtime = 9876543210;
  record.metadata.trackNumber = 3;
  record.metadata.uri = "/path/to/file.flac";
  record.customMeta = {{"key1", "value1"}, {"key2", "value2"}};

  auto data = record.serializeCold();

  // Verify cold view can parse it
  TrackView view{TrackId{0}, std::span<std::byte const>{}, std::as_bytes(std::span{data})};
  CHECK(view.property().fileSize() == 2000);
  CHECK(view.property().mtime() == 9876543210);
  CHECK(view.metadata().trackNumber() == 3);

  // Verify custom meta
  std::vector<std::pair<std::string, std::string>> result;
  for (auto const& [k, v] : view.custom()) {
    result.emplace_back(std::string{k}, std::string{v});
  }
  CHECK(result.size() == 2);
  CHECK(result[0].first == "key1");
  CHECK(result[0].second == "value1");
  CHECK(result[1].first == "key2");
  CHECK(result[1].second == "value2");
}

TEST_CASE("TrackRecord - customMeta field")
{
  TrackRecord record;
  record.customMeta = {{"replaygain_track_gain_db", "-6.5"}, {"isrc", "USSM19999999"}};

  CHECK(record.customMeta.size() == 2);
  CHECK(record.customMeta[0].first == "replaygain_track_gain_db");
  CHECK(record.customMeta[0].second == "-6.5");
}

TEST_CASE("TrackRecord - Cold struct default values")
{
  TrackRecord record;

  CHECK(record.property.fileSize == 0);
  CHECK(record.property.mtime == 0);
  CHECK(record.metadata.trackNumber == 0);
  CHECK(record.metadata.totalTracks == 0);
  CHECK(record.metadata.discNumber == 0);
  CHECK(record.metadata.totalDiscs == 0);
  CHECK(record.metadata.uri.empty());
}
