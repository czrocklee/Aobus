// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

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

  auto parsed = file->loadTrack();

  CHECK(parsed.record.metadata.title == "Test Title");
  CHECK(parsed.record.metadata.artist == "Test Artist");
  CHECK(parsed.record.metadata.album == "Test Album");
  CHECK(parsed.record.metadata.genre == "Rock");
  CHECK(parsed.record.metadata.trackNumber == 1);
  CHECK(parsed.record.metadata.year == 2024);
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

  auto parsed = file->loadTrack();

  CHECK(parsed.record.metadata.title == "HiRes Title");
  CHECK(parsed.record.metadata.artist == "HiRes Artist");
  CHECK(parsed.record.metadata.album == "HiRes Album");
  CHECK(parsed.record.metadata.genre == "Electronic");
  CHECK(parsed.record.metadata.trackNumber == 2);
  CHECK(parsed.record.metadata.year == 2025);
}

// ============================================================================
// Tag Reading Tests - Audio Properties
// ============================================================================

TEST_CASE("Tag reading - audio properties", "[tag][integration]")
{
  auto format = GENERATE("flac", "m4a", "mp3");
  auto path = kTestDataDir / ("basic_metadata." + std::string{format});

  auto file = rs::tag::File::open(path);
  auto parsed = file->loadTrack();

  // Duration ~1 second sine wave (allow some tolerance for encoding)
  CHECK(parsed.record.property.durationMs >= 950);
  CHECK(parsed.record.property.durationMs <= 1050);

  // Standard sample rates
  CHECK(parsed.record.property.sampleRate == 44100);

  // Stereo
  CHECK(parsed.record.property.channels == 2);

  // Bit depth (FLAC 16-bit, M4A/MP3 vary but should be 16+)
  CHECK(parsed.record.property.bitDepth >= 16);

  // Bitrate (MP3 ~128kbps, M4A/AAC ~64-256kbps, FLAC varies)
  CHECK(parsed.record.property.bitrate >= 56000);
}

TEST_CASE("Tag reading - hires audio properties", "[tag][integration]")
{
  auto format = GENERATE("flac", "m4a", "mp3");
  auto path = kTestDataDir / ("hires." + std::string{format});

  auto file = rs::tag::File::open(path);
  auto parsed = file->loadTrack();

  // Duration ~1 second sine wave (allow some tolerance for encoding)
  CHECK(parsed.record.property.durationMs >= 950);
  CHECK(parsed.record.property.durationMs <= 1050);

  // HiRes sample rates
  if (std::string{format} == "mp3") {
    // MP3: 48kHz for hi-res
    CHECK(parsed.record.property.sampleRate == 48000);
    // MP3 is always 16-bit
    CHECK(parsed.record.property.bitDepth == 16);
    // MP3 hi-res: 320kbps
    CHECK(parsed.record.property.bitrate >= 300000);
    CHECK(parsed.record.property.bitrate <= 350000);
  } else if (std::string{format} == "flac") {
    // FLAC: 96kHz for hi-res
    CHECK(parsed.record.property.sampleRate == 96000);
    // FLAC hi-res: 24-bit
    CHECK(parsed.record.property.bitDepth == 24);
    // FLAC bitrate varies
    CHECK(parsed.record.property.bitrate >= 500000);
  } else {
    // M4A: ALAC 96kHz for hi-res
    CHECK(parsed.record.property.sampleRate == 96000);
    // ALAC is lossless, 24-bit
    CHECK(parsed.record.property.bitDepth == 24);
  }

  // Stereo
  CHECK(parsed.record.property.channels == 2);
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

  auto parsed = file->loadTrack();

  // Embedded cover art should be non-empty
  CHECK(!parsed.embeddedCoverArt.empty());
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

  auto parsed = file->loadTrack();

  // Empty files should still have audio properties
  CHECK(parsed.record.property.durationMs > 0);
  CHECK(parsed.record.property.sampleRate > 0);
  CHECK(parsed.record.property.channels > 0);

  // But metadata should be empty
  CHECK(parsed.record.metadata.title.empty());
  CHECK(parsed.record.metadata.artist.empty());
  CHECK(parsed.record.metadata.album.empty());
  CHECK(parsed.record.metadata.genre.empty());
  CHECK(parsed.record.metadata.trackNumber == 0);
  CHECK(parsed.record.metadata.year == 0);
}