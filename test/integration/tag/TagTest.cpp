// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <rs/core/ResourceStore.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>
#include <rs/tag/File.h>

#include <filesystem>

namespace fs = std::filesystem;

// TagTestDataDir is passed via CMake compile definition
static fs::path const kTestDataDir = fs::path{TAG_TEST_DATA_DIR};

// ============================================================================
// Tag Reading Tests - Basic Metadata
// ============================================================================

TEST_CASE("Tag reading - basic metadata", "[tag][integration]")
{
  auto format = GENERATE("flac", "m4a", "mp3");
  auto path = kTestDataDir / ("basic_metadata." + std::string{format});

  auto file = rs::tag::File::open(path);
  REQUIRE(file != nullptr);

  auto builder = file->loadTrack();
  auto record = builder.record();

  CHECK(record.metadata.title == "Test Title");
  CHECK(record.metadata.artist == "Test Artist");
  CHECK(record.metadata.album == "Test Album");
  CHECK(record.metadata.genre == "Rock");
  CHECK(record.metadata.trackNumber == 1);
  CHECK(record.metadata.year == 2024);
}

// ============================================================================
// Tag Reading Tests - HiRes Metadata
// ============================================================================

TEST_CASE("Tag reading - hires metadata", "[tag][integration]")
{
  auto format = GENERATE("flac", "m4a", "mp3");
  auto path = kTestDataDir / ("hires." + std::string{format});

  auto file = rs::tag::File::open(path);
  REQUIRE(file != nullptr);

  auto builder = file->loadTrack();
  auto record = builder.record();

  CHECK(record.metadata.title == "HiRes Title");
  CHECK(record.metadata.artist == "HiRes Artist");
  CHECK(record.metadata.album == "HiRes Album");
  CHECK(record.metadata.genre == "Electronic");
  CHECK(record.metadata.trackNumber == 2);
  CHECK(record.metadata.year == 2025);
}

// ============================================================================
// Tag Reading Tests - Audio Properties
// ============================================================================

TEST_CASE("Tag reading - audio properties", "[tag][integration]")
{
  auto format = GENERATE("flac", "m4a", "mp3");
  auto path = kTestDataDir / ("basic_metadata." + std::string{format});

  auto file = rs::tag::File::open(path);
  auto builder = file->loadTrack();
  auto record = builder.record();

  // Duration ~1 second sine wave (allow some tolerance for encoding)
  CHECK(record.property.durationMs >= 950);
  CHECK(record.property.durationMs <= 1050);

  // Standard sample rates
  CHECK(record.property.sampleRate == 44100);

  // Stereo
  CHECK(record.property.channels == 2);

  // Bit depth (FLAC 16-bit, M4A/MP3 vary but should be 16+)
  CHECK(record.property.bitDepth >= 16);

  // Bitrate (MP3 ~128kbps, M4A/AAC ~64-256kbps, FLAC varies)
  CHECK(record.property.bitrate >= 56000);
}

TEST_CASE("Tag reading - hires audio properties", "[tag][integration]")
{
  auto format = GENERATE("flac", "m4a", "mp3");
  auto path = kTestDataDir / ("hires." + std::string{format});

  auto file = rs::tag::File::open(path);
  auto builder = file->loadTrack();
  auto record = builder.record();

  // Duration ~1 second sine wave (allow some tolerance for encoding)
  CHECK(record.property.durationMs >= 950);
  CHECK(record.property.durationMs <= 1050);

  // HiRes sample rates
  if (std::string{format} == "mp3") {
    // MP3: 48kHz for hi-res
    CHECK(record.property.sampleRate == 48000);
    // MP3 is always 16-bit
    CHECK(record.property.bitDepth == 16);
    // MP3 hi-res: 320kbps
    CHECK(record.property.bitrate >= 300000);
    CHECK(record.property.bitrate <= 350000);
  } else if (std::string{format} == "flac") {
    // FLAC: 96kHz for hi-res
    CHECK(record.property.sampleRate == 96000);
    // FLAC hi-res: 24-bit
    CHECK(record.property.bitDepth == 24);
    // FLAC bitrate varies
    CHECK(record.property.bitrate >= 500000);
  } else {
    // M4A: ALAC 96kHz for hi-res
    CHECK(record.property.sampleRate == 96000);
    // ALAC is lossless, 24-bit
    CHECK(record.property.bitDepth == 24);
  }

  // Stereo
  CHECK(record.property.channels == 2);
}

// ============================================================================
// Cover Art Extraction Tests
// ============================================================================

TEST_CASE("Cover art extraction", "[tag][integration]")
{
  auto format = GENERATE("flac", "m4a", "mp3");
  auto path = kTestDataDir / ("with_cover." + std::string{format});

  auto file = rs::tag::File::open(path);
  REQUIRE(file != nullptr);

  auto builder = file->loadTrack();

  // Create temp LMDB environment to test cover art serialization
  auto tempDir = fs::temp_directory_path() / "rs_tag_test_XXXXXX";
  fs::create_directories(tempDir);
  auto env = rs::lmdb::Environment{tempDir, {.flags = MDB_CREATE, .maxDatabases = 20}};
  auto wtxn = rs::lmdb::WriteTransaction{env};
  auto dict = rs::core::DictionaryStore{rs::lmdb::Database{wtxn, "dict"}, wtxn};
  auto resources = rs::core::ResourceStore{rs::lmdb::Database{wtxn, "resources"}};

  auto [hotData, coldData] = builder.serialize(wtxn, dict, resources);

  CHECK(!hotData.empty());
  CHECK(!coldData.empty());
  CHECK(builder.record().metadata.coverArtId > 0);

  // Cleanup - transaction will abort if not committed
  fs::remove_all(tempDir);
}

// ============================================================================
// Empty/Missing Metadata Tests
// ============================================================================

TEST_CASE("Tag reading - empty metadata", "[tag][integration]")
{
  auto format = GENERATE("flac", "m4a", "mp3");
  auto path = kTestDataDir / ("empty." + std::string{format});

  auto file = rs::tag::File::open(path);
  REQUIRE(file != nullptr);

  auto builder = file->loadTrack();
  auto record = builder.record();

  // Empty files should still have audio properties
  CHECK(record.property.durationMs > 0);
  CHECK(record.property.sampleRate > 0);
  CHECK(record.property.channels > 0);

  // But metadata should be empty
  CHECK(record.metadata.title.empty());
  CHECK(record.metadata.artist.empty());
  CHECK(record.metadata.album.empty());
  CHECK(record.metadata.genre.empty());
  CHECK(record.metadata.trackNumber == 0);
  CHECK(record.metadata.year == 0);
}