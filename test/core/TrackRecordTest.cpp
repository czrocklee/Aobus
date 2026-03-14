/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <catch2/catch.hpp>

#include <rs/core/TrackLayout.h>
#include <rs/core/TrackRecord.h>

#include <cstring>

using rs::core::DictionaryId;
using rs::core::TrackHeader;
using rs::core::TrackRecord;
using rs::core::TrackView;

TEST_CASE("TrackRecord - Default Constructor")
{
  TrackRecord record;

  CHECK(record.property.fileSize == 0);
  CHECK(record.property.mtime == 0);
  CHECK(record.property.durationMs == 0);
  CHECK(record.property.bitrate == 0);
  CHECK(record.property.sampleRate == 0);
  CHECK(record.metadata.year == 0);
  CHECK(record.metadata.trackNumber == 0);
  CHECK(record.property.channels == 0);
  CHECK(record.property.bitDepth == 0);
  CHECK(record.property.rating == 0);
  CHECK(record.metadata.title.empty());
  CHECK(record.metadata.uri.empty());
  CHECK(record.metadata.artist.empty());
  CHECK(record.metadata.album.empty());
  CHECK(record.metadata.albumArtist.empty());
  CHECK(record.metadata.genre.empty());
  CHECK(record.tags.ids.empty());
}

TEST_CASE("TrackRecord - Field Assignment")
{
  TrackRecord record;
  record.metadata.title = "Test Title";
  record.metadata.uri = "/path/to/track.flac";
  record.metadata.artist = "Test Artist";
  record.metadata.album = "Test Album";
  record.metadata.genre = "Rock";
  record.metadata.year = 2020;
  record.metadata.trackNumber = 5;
  record.property.durationMs = 180000;
  record.property.bitrate = 320000;
  record.property.sampleRate = 44100;
  record.property.channels = 2;
  record.property.bitDepth = 16;
  record.property.rating = 4;
  record.property.fileSize = 10000000;
  record.property.mtime = 1234567890;

  CHECK(record.metadata.title == "Test Title");
  CHECK(record.metadata.uri == "/path/to/track.flac");
  CHECK(record.metadata.artist == "Test Artist");
  CHECK(record.metadata.album == "Test Album");
  CHECK(record.metadata.genre == "Rock");
  CHECK(record.metadata.year == 2020);
  CHECK(record.metadata.trackNumber == 5);
  CHECK(record.property.durationMs == 180000);
  CHECK(record.property.bitrate == 320000);
  CHECK(record.property.sampleRate == 44100);
  CHECK(record.property.channels == 2);
  CHECK(record.property.bitDepth == 16);
  CHECK(record.property.rating == 4);
}

TEST_CASE("TrackRecord - Serialize Empty Record")
{
  TrackRecord record;
  auto data = record.serialize();

  CHECK(data.size() >= sizeof(TrackHeader));
  CHECK(!data.empty());
}

TEST_CASE("TrackRecord - Serialize With Strings")
{
  TrackRecord record;
  record.metadata.title = "Hello World";
  record.metadata.uri = "/music/test.flac";
  record.metadata.year = 2021;
  record.property.durationMs = 240000;

  auto data = record.serialize();

  // Verify header size
  CHECK(data.size() >= sizeof(TrackHeader));

  // Parse the serialized data back
  const auto* header = reinterpret_cast<const TrackHeader*>(data.data());

  CHECK(header->titleLen == 11); // "Hello World"
  CHECK(header->uriLen == 16);   // "/music/test.flac"
  CHECK(header->year == 2021);
  CHECK(header->durationMs == 240000);

  // Verify strings are in the payload
  auto payloadStart = reinterpret_cast<const char*>(data.data()) + sizeof(TrackHeader);
  CHECK(std::strncmp(payloadStart, "Hello World", 11) == 0);
}

TEST_CASE("TrackRecord - Header Method")
{
  TrackRecord record;
  record.metadata.year = 1999;
  record.metadata.trackNumber = 7;
  record.property.durationMs = 300000;
  record.property.channels = 2;
  record.property.bitDepth = 24;
  record.property.rating = 5;

  auto header = record.header();

  CHECK(header.year == 1999);
  CHECK(header.trackNumber == 7);
  CHECK(header.durationMs == 300000);
  CHECK(header.channels == 2);
  CHECK(header.bitDepth == 24);
  CHECK(header.rating == 5);
}

TEST_CASE("TrackRecord - Serialize With Special Characters")
{
  TrackRecord record;
  record.metadata.title = "Test: \"Quotes\" & 'Apostrophes'";
  record.metadata.uri = "/path/with spaces/file.mp3";

  auto data = record.serialize();

  const auto* header = reinterpret_cast<const TrackHeader*>(data.data());
  CHECK(header->titleLen == record.metadata.title.size());
  CHECK(header->uriLen == record.metadata.uri.size());
}

TEST_CASE("TrackRecord - Serialize Preserves Data")
{
  TrackRecord record;
  record.metadata.title = "Test";
  record.metadata.uri = "/test";
  record.property.fileSize = 12345;
  record.property.mtime = 9876543210;

  auto data = record.serialize();
  auto data2 = record.serialize();

  // Multiple serializations should produce same size and content
  CHECK(data.size() == data2.size());
  CHECK(data == data2);
}

TEST_CASE("TrackRecord - Tag Serialization - Empty Tags")
{
  TrackRecord record;
  record.metadata.title = "Test";
  record.metadata.uri = "/test";

  auto data = record.serialize();

  const auto* header = reinterpret_cast<const TrackHeader*>(data.data());
  CHECK(header->tagCount == 0);
  CHECK(header->tagBloom == 0);
}

TEST_CASE("TrackRecord - Tag Serialization - With Tags")
{
  TrackRecord record;
  record.metadata.title = "Test";
  record.metadata.uri = "/test";
  record.tags.ids = {DictionaryId{10}, DictionaryId{20}, DictionaryId{30}};

  auto data = record.serialize();

  const auto* header = reinterpret_cast<const TrackHeader*>(data.data());
  CHECK(header->tagCount == 3);
  CHECK(header->tagBloom != 0); // Bloom should be computed from tag IDs

  // Verify tag IDs are in the payload
  auto payloadStart = reinterpret_cast<const char*>(data.data()) + sizeof(TrackHeader);

  const auto* tagIdsPtr = reinterpret_cast<const std::uint32_t*>(payloadStart + header->tagsOffset);
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

  auto data = record.serialize();

  const auto* header = reinterpret_cast<const TrackHeader*>(data.data());
  CHECK(header->tagCount == 1);

  auto payloadStart = reinterpret_cast<const char*>(data.data()) + sizeof(TrackHeader);
  const auto* tagIdPtr = reinterpret_cast<const std::uint32_t*>(payloadStart + header->tagsOffset);
  CHECK(*tagIdPtr == 42);
}

TEST_CASE("TrackRecord - Header Method With Tags")
{
  TrackRecord record;
  record.metadata.title = "Test";
  record.metadata.uri = "/test";
  record.tags.ids = {DictionaryId{1}, DictionaryId{2}, DictionaryId{3}, DictionaryId{4}, DictionaryId{5}};

  auto header = record.header();

  CHECK(header.tagCount == 5);
  CHECK(header.tagBloom != 0);
}
