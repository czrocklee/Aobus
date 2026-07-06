// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/lmdb/TestUtils.h"
#include <ao/CoreIds.h>
#include <ao/library/CoverArt.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackView.h>
#include <ao/tag/TagFile.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <lmdb.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;

namespace ao::tag::test
{
  namespace
  {
    fs::path const kTestDataDir = fs::path{TAG_TEST_DATA_DIR};

    struct LoadedTrack final
    {
      std::unique_ptr<TagFile> filePtr;
      library::TrackBuilder builder;
    };

    LoadedTrack loadTrack(fs::path const& path)
    {
      auto fileResult = TagFile::open(path);
      REQUIRE(fileResult);
      REQUIRE(*fileResult != nullptr);

      auto trackResult = (*fileResult)->loadTrack();
      REQUIRE(trackResult);
      return {.filePtr = std::move(*fileResult), .builder = *trackResult};
    }

    bool hasPngSignature(std::span<std::byte const> bytes)
    {
      return bytes.size() >= 8 && bytes[0] == std::byte{0x89} && bytes[1] == std::byte{0x50} &&
             bytes[2] == std::byte{0x4E} && bytes[3] == std::byte{0x47} && bytes[4] == std::byte{0x0D} &&
             bytes[5] == std::byte{0x0A} && bytes[6] == std::byte{0x1A} && bytes[7] == std::byte{0x0A};
    }

    std::uint32_t readPngBigEndian32(std::span<std::byte const> bytes, std::size_t offset)
    {
      REQUIRE(bytes.size() >= offset + 4);

      return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
             (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
             (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) | static_cast<std::uint32_t>(bytes[offset + 3]);
    }

    void checkOnePixelPng(std::span<std::byte const> bytes)
    {
      REQUIRE(bytes.size() >= 24);
      CHECK(hasPngSignature(bytes));
      CHECK(readPngBigEndian32(bytes, 16) == 1U);
      CHECK(readPngBigEndian32(bytes, 20) == 1U);
    }
  } // namespace

  TEST_CASE("TagReader - basic fixture exposes metadata", "[tag][integration][metadata]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3", "wav");
    auto const path = kTestDataDir / ("basic_metadata." + std::string{format});

    auto loaded = loadTrack(path);
    auto& builder = loaded.builder;
    auto& meta = builder.metadata();

    CHECK(meta.title() == "Test Title");
    CHECK(meta.artist() == "Test Artist");
    CHECK(meta.album() == "Test Album");
    CHECK(meta.genre() == "Rock");
    CHECK(meta.year() == 2024);

    if (std::string_view{format} != "wav")
    {
      CHECK(meta.composer() == "Test Composer");
      CHECK(meta.work() == "Symphony No. 5");
      CHECK(meta.trackNumber() == 1);
    }
  }

  // ============================================================================
  // Tag Reading Tests - HiRes Metadata
  // ============================================================================
  TEST_CASE("TagReader - hires fixture exposes metadata", "[tag][integration][metadata]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3", "wav");
    auto const path = kTestDataDir / ("hires." + std::string{format});

    auto loaded = loadTrack(path);
    auto& builder = loaded.builder;
    auto& meta = builder.metadata();

    CHECK(meta.title() == "HiRes Title");
    CHECK(meta.artist() == "HiRes Artist");
    CHECK(meta.album() == "HiRes Album");
    CHECK(meta.genre() == "Electronic");
    CHECK(meta.year() == 2025);

    if (std::string_view{format} != "wav")
    {
      CHECK(meta.composer() == "HiRes Composer");
      CHECK(meta.work() == "The Four Seasons");
      CHECK(meta.trackNumber() == 2);
    }
  }

  TEST_CASE("TagReader - classical fixture exposes metadata", "[tag][integration][metadata][classical]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3");
    CAPTURE(format);
    auto const path = kTestDataDir / ("classical_metadata." + std::string{format});

    auto loaded = loadTrack(path);
    auto& meta = loaded.builder.metadata();

    CHECK(meta.title() == "Classical Fixture");
    CHECK(meta.artist() == "Classical Artist");
    CHECK(meta.album() == "Classical Album");
    CHECK(meta.genre() == "Classical");
    CHECK(meta.composer() == "Fixture Composer");
    CHECK(meta.conductor() == "Fixture Conductor");
    CHECK(meta.ensemble() == "Fixture Ensemble");
    CHECK(meta.soloist() == "Fixture Soloist");
    CHECK(meta.work() == "Fixture Work");
    CHECK(meta.movement() == "Fixture Movement");
    CHECK(meta.movementNumber() == 2);
    CHECK(meta.movementTotal() == 4);
    CHECK(meta.trackNumber() == 3);
    CHECK(meta.trackTotal() == 9);
    CHECK(meta.year() == 2026);
  }

  TEST_CASE("TagReader - classical fallback fixture maps orchestra fields", "[tag][integration][metadata][classical]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3");
    CAPTURE(format);
    auto const path = kTestDataDir / ("classical_fallback." + std::string{format});

    auto loaded = loadTrack(path);
    auto& meta = loaded.builder.metadata();

    CHECK(meta.title() == "Classical Fallback");
    CHECK(meta.ensemble() == "Fixture Fallback Ensemble");

    if (std::string_view{format} == "flac")
    {
      CHECK(meta.soloist() == "Fixture Fallback Soloist");
    }
  }

  // ============================================================================
  // Tag Reading Tests - Audio Properties
  // ============================================================================
  TEST_CASE("TagReader - basic fixture exposes audio properties", "[tag][integration][property]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3", "wav");
    auto const path = kTestDataDir / ("basic_metadata." + std::string{format});

    auto loaded = loadTrack(path);
    auto& builder = loaded.builder;
    auto& prop = builder.property();

    // Duration ~1 second sine wave (allow some tolerance for encoding)
    CHECK(prop.duration() >= std::chrono::milliseconds{950});
    CHECK(prop.duration() <= std::chrono::milliseconds{1050});

    // Standard sample rates
    CHECK(prop.sampleRate() == 44100);

    // Stereo
    CHECK(prop.channels() == 2);

    // Bit depth (FLAC 16-bit, M4A/MP3 vary but should be 16+)
    CHECK(prop.bitDepth() >= 16);

    // Bitrate (MP3 ~128kbps, M4A/AAC ~64-256kbps, FLAC varies)
    CHECK(prop.bitrate() >= 56000);
  }

  TEST_CASE("TagReader - hires fixture exposes audio properties", "[tag][integration][property]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3", "wav");
    auto const path = kTestDataDir / ("hires." + std::string{format});

    auto loaded = loadTrack(path);
    auto& builder = loaded.builder;
    auto& prop = builder.property();

    // Duration ~1 second sine wave (allow some tolerance for encoding)
    CHECK(prop.duration() >= std::chrono::milliseconds{950});
    CHECK(prop.duration() <= std::chrono::milliseconds{1050});

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
    else if (std::string{format} == "wav")
    {
      CHECK(prop.sampleRate() == 96000);
      CHECK(prop.bitDepth() == 24);
      CHECK(prop.bitrate() >= 4000000);
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
  TEST_CASE("TagReader - cover art fixture exposes primary artwork", "[tag][integration][cover-art]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3");
    auto const path = kTestDataDir / ("with_cover." + std::string{format});

    auto loaded = loadTrack(path);
    auto& builder = loaded.builder;

    // Create temp LMDB environment to test cover art serialization
    auto const tempDir = ao::test::TempDir{};
    auto env = lmdb::test::openEnvironment(tempDir.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = lmdb::test::beginWriteTransaction(env);
    auto dict = library::DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
    auto resources = library::ResourceStore{lmdb::test::openDatabase(wtxn, "resources")};

    auto serializeResult = builder.serialize(wtxn, dict, resources);
    REQUIRE(serializeResult);
    auto const [hotData, coldData] = *serializeResult;

    CHECK(!hotData.empty());
    CHECK(!coldData.empty());

    // Check cover art is present via TrackView
    auto const view = library::TrackView{hotData, coldData};
    REQUIRE(view.coverArt().count() == 1);
    auto const expectedType =
      std::string_view{format} == "m4a" ? library::PictureType::FrontCover : library::PictureType::Other;
    auto const cover = view.coverArt().at(0);
    CHECK(cover.type == expectedType);
    CHECK(cover.resourceId != kInvalidResourceId);

    auto const optPrimary = view.coverArt().primary();
    REQUIRE(optPrimary);
    CHECK(optPrimary->type == cover.type);
    CHECK(optPrimary->resourceId == cover.resourceId);

    auto const optStoredBytes = resources.writer(wtxn).get(cover.resourceId);
    REQUIRE(optStoredBytes);
    CHECK_FALSE(optStoredBytes->empty());
    checkOnePixelPng(*optStoredBytes);
  }

  // ============================================================================
  // Empty/Missing Metadata Tests
  // ============================================================================
  TEST_CASE("TagReader - empty fixture exposes empty metadata", "[tag][integration][metadata]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3", "wav");
    auto const path = kTestDataDir / ("empty." + std::string{format});

    auto loaded = loadTrack(path);
    auto& builder = loaded.builder;
    auto& meta = builder.metadata();
    auto& prop = builder.property();

    // Empty files should still have audio properties
    CHECK(prop.duration() > std::chrono::milliseconds{0});
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
