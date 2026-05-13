// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/library/ResourceStore.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/tag/TagFile.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <filesystem>

namespace fs = std::filesystem;

namespace ao::tag::test
{
  namespace
  {
    fs::path const kTestDataDir = fs::path{TAG_TEST_DATA_DIR};
  }

  TEST_CASE("Tag reading - basic metadata", "[tag][integration]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3");
    auto const path = kTestDataDir / ("basic_metadata." + std::string{format});

    auto const file = tag::TagFile::open(path);
    REQUIRE(file != nullptr);

    auto builder = file->loadTrack();
    auto& meta = builder.metadata();

    CHECK(meta.title() == "Test Title");
    CHECK(meta.artist() == "Test Artist");
    CHECK(meta.album() == "Test Album");
    CHECK(meta.composer() == "Test Composer");
    CHECK(meta.work() == "Symphony No. 5");
    CHECK(meta.genre() == "Rock");
    CHECK(meta.trackNumber() == 1);
    CHECK(meta.year() == 2024);
  }

  // ============================================================================
  // Tag Reading Tests - HiRes Metadata
  // ============================================================================

  TEST_CASE("Tag reading - hires metadata", "[tag][integration]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3");
    auto const path = kTestDataDir / ("hires." + std::string{format});

    auto const file = tag::TagFile::open(path);
    REQUIRE(file != nullptr);

    auto builder = file->loadTrack();
    auto& meta = builder.metadata();

    CHECK(meta.title() == "HiRes Title");
    CHECK(meta.artist() == "HiRes Artist");
    CHECK(meta.album() == "HiRes Album");
    CHECK(meta.composer() == "HiRes Composer");
    CHECK(meta.work() == "The Four Seasons");
    CHECK(meta.genre() == "Electronic");
    CHECK(meta.trackNumber() == 2);
    CHECK(meta.year() == 2025);
  }

  // ============================================================================
  // Tag Reading Tests - Audio Properties
  // ============================================================================

  TEST_CASE("Tag reading - audio properties", "[tag][integration]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3");
    auto const path = kTestDataDir / ("basic_metadata." + std::string{format});

    auto const file = tag::TagFile::open(path);
    auto builder = file->loadTrack();
    auto& prop = builder.property();

    // Duration ~1 second sine wave (allow some tolerance for encoding)
    CHECK(prop.durationMs() >= 950);
    CHECK(prop.durationMs() <= 1050);

    // Standard sample rates
    CHECK(prop.sampleRate() == 44100);

    // Stereo
    CHECK(prop.channels() == 2);

    // Bit depth (FLAC 16-bit, M4A/MP3 vary but should be 16+)
    CHECK(prop.bitDepth() >= 16);

    // Bitrate (MP3 ~128kbps, M4A/AAC ~64-256kbps, FLAC varies)
    CHECK(prop.bitrate() >= 56000);
  }

  TEST_CASE("Tag reading - hires audio properties", "[tag][integration]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3");
    auto const path = kTestDataDir / ("hires." + std::string{format});

    auto const file = tag::TagFile::open(path);
    auto builder = file->loadTrack();
    auto& prop = builder.property();

    // Duration ~1 second sine wave (allow some tolerance for encoding)
    CHECK(prop.durationMs() >= 950);
    CHECK(prop.durationMs() <= 1050);

    // HiRes sample rates
    if (std::string{format} == "mp3")
    {
      // MP3: 48kHz for hi-res
      CHECK(prop.sampleRate() == 48000);
      // MP3 is always 16-bit
      CHECK(prop.bitDepth() == 16);
      // MP3 hi-res: 320kbps
      CHECK(prop.bitrate() >= 300000);
      CHECK(prop.bitrate() <= 350000);
    }
    else if (std::string{format} == "flac")
    {
      // FLAC: 96kHz for hi-res
      CHECK(prop.sampleRate() == 96000);
      // FLAC hi-res: 24-bit
      CHECK(prop.bitDepth() == 24);
      // FLAC bitrate varies
      CHECK(prop.bitrate() >= 500000);
    }
    else
    {
      // M4A: ALAC 96kHz for hi-res
      CHECK(prop.sampleRate() == 96000);
      // ALAC is lossless, 24-bit
      CHECK(prop.bitDepth() == 24);
    }

    // Stereo
    CHECK(prop.channels() == 2);
  }

  // ============================================================================
  // Cover Art Extraction Tests
  // ============================================================================

  TEST_CASE("Cover art extraction", "[tag][integration]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3");
    auto const path = kTestDataDir / ("with_cover." + std::string{format});

    auto const file = tag::TagFile::open(path);
    REQUIRE(file != nullptr);

    auto builder = file->loadTrack();

    // Create temp LMDB environment to test cover art serialization
    auto const tempDir = fs::temp_directory_path() / "rs_tag_test_XXXXXX";
    fs::create_directories(tempDir);
    auto env = lmdb::Environment{tempDir, {.flags = MDB_CREATE, .maxDatabases = 20}};
    auto wtxn = lmdb::WriteTransaction{env};
    auto dict = library::DictionaryStore{lmdb::Database{wtxn, "dict"}, wtxn};
    auto resources = library::ResourceStore{lmdb::Database{wtxn, "resources"}};

    auto const [hotData, coldData] = builder.serialize(wtxn, dict, resources);

    CHECK(!hotData.empty());
    CHECK(!coldData.empty());

    // Check cover art ID directly from serialized cold header
    auto const* const coldHdr = reinterpret_cast<library::TrackColdHeader const*>(coldData.data());
    CHECK(coldHdr->coverArtId > 0);

    // Cleanup - transaction will abort if not committed
    fs::remove_all(tempDir);
  }

  // ============================================================================
  // Empty/Missing Metadata Tests
  // ============================================================================

  TEST_CASE("Tag reading - empty metadata", "[tag][integration]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3");
    auto const path = kTestDataDir / ("empty." + std::string{format});

    auto const file = tag::TagFile::open(path);
    REQUIRE(file != nullptr);

    auto builder = file->loadTrack();
    auto& meta = builder.metadata();
    auto& prop = builder.property();

    // Empty files should still have audio properties
    CHECK(prop.durationMs() > 0);
    CHECK(prop.sampleRate() > 0);
    CHECK(prop.channels() > 0);

    // But metadata should be empty
    CHECK(meta.title().empty());
    CHECK(meta.artist().empty());
    CHECK(meta.album().empty());
    CHECK(meta.genre().empty());
    CHECK(meta.trackNumber() == 0);
    CHECK(meta.year() == 0);
  }
} // namespace ao::tag::test
