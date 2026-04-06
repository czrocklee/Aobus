// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <rs/core/TrackRecord.h>
#include <rs/core/TrackLayout.h>
#include <rs/core/TrackView.h>

#include <cstring>
#include <vector>

using rs::core::TrackColdHeader;
using rs::core::TrackHotHeader;
using rs::core::TrackRecord;
using rs::core::TrackView;

TEST_CASE("TrackRecord - Default Constructor")
{
  auto record = TrackRecord{};

  CHECK(record.metadata.title.empty());
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
  CHECK(record.property.uri.empty());
  CHECK(record.property.fileSize == 0);
  CHECK(record.property.mtime == 0);
  CHECK(record.property.durationMs == 0);
  CHECK(record.property.bitrate == 0);
  CHECK(record.property.sampleRate == 0);
  CHECK(record.property.codecId == 0);
  CHECK(record.property.channels == 0);
  CHECK(record.property.bitDepth == 0);
  CHECK(record.metadata.rating == 0);
  CHECK(record.tags.names.empty());
}

TEST_CASE("TrackRecord - Field Assignment")
{
  auto record = TrackRecord{};
  record.metadata.title = "Test Title";
  record.property.uri = "/path/to/track.flac";
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
  record.metadata.rating = 4;

  CHECK(record.metadata.title == "Test Title");
  CHECK(record.property.uri == "/path/to/track.flac");
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
  CHECK(record.metadata.rating == 4);
}

TEST_CASE("TrackRecord - custom.pairs field")
{
  auto record = TrackRecord{};
  record.custom.pairs = {{"replaygain_track_gain_db", "-6.5"}, {"isrc", "USSM19999999"}};

  CHECK(record.custom.pairs.size() == 2);
  CHECK(record.custom.pairs[0].first == "replaygain_track_gain_db");
  CHECK(record.custom.pairs[0].second == "-6.5");
}

TEST_CASE("TrackRecord - Cold struct default values")
{
  auto record = TrackRecord{};

  CHECK(record.property.fileSize == 0);
  CHECK(record.property.mtime == 0);
  CHECK(record.metadata.trackNumber == 0);
  CHECK(record.metadata.totalTracks == 0);
  CHECK(record.metadata.discNumber == 0);
  CHECK(record.metadata.totalDiscs == 0);
  CHECK(record.property.uri.empty());
}

TEST_CASE("TrackRecord - Constructor validates both hot and cold", "[core][track]")
{
  // Verify that isHotValid and isColdValid work correctly
  auto hotHeader = TrackHotHeader{};
  hotHeader.titleLen = 0;
  hotHeader.tagLen = 0;
  auto hotData = std::vector<std::byte>(sizeof(TrackHotHeader));
  std::memcpy(hotData.data(), &hotHeader, sizeof(TrackHotHeader));

  auto coldHeader = TrackColdHeader{};
  coldHeader.fileSizeLo = static_cast<std::uint32_t>(1000 & 0xFFFFFFFF);
  coldHeader.fileSizeHi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(1000) >> 32);
  auto coldData = std::vector<std::byte>(sizeof(TrackColdHeader));
  std::memcpy(coldData.data(), &coldHeader, sizeof(TrackColdHeader));

  // Hot-only view
  {
    auto view = TrackView{hotData, {}};
    CHECK(view.isHotValid());
    CHECK(!view.isColdValid());
  }

  // Cold-only view
  {
    auto view = TrackView{{}, coldData};
    CHECK(!view.isHotValid());
    CHECK(view.isColdValid());
  }

  // Both-valid view
  {
    auto view = TrackView{hotData, coldData};
    CHECK(view.isHotValid());
    CHECK(view.isColdValid());
  }
}