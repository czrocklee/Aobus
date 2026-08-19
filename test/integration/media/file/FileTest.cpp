// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/library/MediaTrack.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/PictureType.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceLayout.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackView.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;

namespace ao::media::file::test
{
  namespace
  {
    fs::path const kTestDataDir = fs::path{AUDIO_TEST_DATA_DIR};

    rt::MediaTrack loadTrack(fs::path const& path)
    {
      auto trackRes = rt::readMediaTrack(path);
      REQUIRE(trackRes);
      return std::move(*trackRes);
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

  TEST_CASE("Media File - basic fixture exposes metadata", "[media][integration][metadata]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3", "wav", "opus");
    auto const path = kTestDataDir / ("basic_metadata." + std::string{format});

    auto loaded = loadTrack(path);
    auto& builder = loaded.builder();
    auto& metadata = builder.metadata();

    CHECK(metadata.title() == "Test Title");
    CHECK(metadata.artist() == "Test Artist");
    CHECK(metadata.album() == "Test Album");
    CHECK(metadata.genre() == "Rock");
    CHECK(metadata.year() == 2024);

    if (std::string_view{format} != "wav")
    {
      CHECK(metadata.composer() == "Test Composer");
      CHECK(metadata.work() == "Symphony No. 5");
      CHECK(metadata.trackNumber() == 1);
    }
  }

  // ============================================================================
  // Media file tests - high-resolution metadata
  // ============================================================================
  TEST_CASE("Media File - hires fixture exposes metadata", "[media][integration][metadata]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3", "wav");
    auto const path = kTestDataDir / ("hires." + std::string{format});

    auto loaded = loadTrack(path);
    auto& builder = loaded.builder();
    auto& metadata = builder.metadata();

    CHECK(metadata.title() == "HiRes Title");
    CHECK(metadata.artist() == "HiRes Artist");
    CHECK(metadata.album() == "HiRes Album");
    CHECK(metadata.genre() == "Electronic");
    CHECK(metadata.year() == 2025);

    if (std::string_view{format} != "wav")
    {
      CHECK(metadata.composer() == "HiRes Composer");
      CHECK(metadata.work() == "The Four Seasons");
      CHECK(metadata.trackNumber() == 2);
    }
  }

  TEST_CASE("Media File - classical fixture exposes metadata", "[media][integration][metadata][classical]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3", "opus");
    CAPTURE(format);
    auto const path = kTestDataDir / ("classical_metadata." + std::string{format});

    auto loaded = loadTrack(path);
    auto& metadata = loaded.builder().metadata();

    CHECK(metadata.title() == "Classical Fixture");
    CHECK(metadata.artist() == "Classical Artist");
    CHECK(metadata.album() == "Classical Album");
    CHECK(metadata.genre() == "Classical");
    CHECK(metadata.composer() == "Fixture Composer");
    CHECK(metadata.conductor() == "Fixture Conductor");
    CHECK(metadata.ensemble() == "Fixture Ensemble");
    CHECK(metadata.soloist() == "Fixture Soloist");
    CHECK(metadata.work() == "Fixture Work");
    CHECK(metadata.movement() == "Fixture Movement");
    CHECK(metadata.movementNumber() == 2);
    CHECK(metadata.movementTotal() == 4);
    CHECK(metadata.trackNumber() == 3);
    CHECK(metadata.trackTotal() == 9);
    CHECK(metadata.year() == 2026);
  }

  TEST_CASE("Media File - classical fallback fixture maps orchestra fields",
            "[media][integration][metadata][classical]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3", "opus");
    CAPTURE(format);
    auto const path = kTestDataDir / ("classical_fallback." + std::string{format});

    auto loaded = loadTrack(path);
    auto& metadata = loaded.builder().metadata();

    CHECK(metadata.title() == "Classical Fallback");
    CHECK(metadata.ensemble() == "Fixture Fallback Ensemble");

    // FLAC and Opus share one Vorbis comment vocabulary, so both map PERFORMER.
    if (std::string_view{format} == "flac" || std::string_view{format} == "opus")
    {
      CHECK(metadata.soloist() == "Fixture Fallback Soloist");
    }
  }

  // ============================================================================
  // Media file tests - audio properties
  // ============================================================================
  TEST_CASE("Media File - basic fixture exposes audio properties", "[media][integration][property]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3", "wav");
    auto const path = kTestDataDir / ("basic_metadata." + std::string{format});

    auto loaded = loadTrack(path);
    auto& builder = loaded.builder();
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

  TEST_CASE("Media File - hires fixture exposes audio properties", "[media][integration][property]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3", "wav");
    auto const path = kTestDataDir / ("hires." + std::string{format});

    auto loaded = loadTrack(path);
    auto& builder = loaded.builder();
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
  TEST_CASE("Media File - cover art fixture exposes primary artwork", "[media][integration][cover-art]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3", "opus");
    auto const path = kTestDataDir / ("with_cover." + std::string{format});

    auto loaded = loadTrack(path);
    auto& builder = loaded.builder();
    auto const libraryUri = path.filename().generic_string();
    builder.property().uri(libraryUri);

    // Create temp LMDB environment to test cover art serialization
    auto const tempDir = ao::test::TempDir{};
    auto musicLibrary = library::test::makeTestMusicLibrary(tempDir.path(), tempDir.path() / "db");
    auto transaction = library::test::writeTransaction(musicLibrary);
    auto serializeRes = library::test::physicalSerializeTrack(builder, transaction, musicLibrary.resources());
    REQUIRE(serializeRes);
    auto const [hotData, coldData] = *serializeRes;

    CHECK(!hotData.empty());
    CHECK(!coldData.empty());

    // Check cover art is present via TrackView
    auto const view = library::TrackView{hotData, coldData};
    REQUIRE(view.coverArt().count() == 1);
    // MP4 covr entries and Opus METADATA_BLOCK_PICTURE name a front cover; the
    // FLAC and ID3 fixtures carry the default role instead.
    auto const formatName = std::string_view{format};
    auto const expectedType =
      (formatName == "m4a" || formatName == "opus") ? PictureType::FrontCover : PictureType::Other;
    auto const cover = view.coverArt().at(0);
    CHECK(cover.type == expectedType);
    CHECK(cover.resourceId != kInvalidResourceId);

    auto const optPrimary = view.coverArt().primary();
    REQUIRE(optPrimary);
    CHECK(optPrimary->type == cover.type);
    CHECK(optPrimary->resourceId == cover.resourceId);

    // The row describes the picture instead of holding it, so the check is that
    // its identity is the digest of the payload the reader handed out: one
    // encoded image stored in two container formats must yield one digest.
    auto const& pending = builder.coverArt().entries();
    REQUIRE(pending.size() == 1);
    auto const pictureBytes = std::get<std::span<std::byte const>>(pending.front().source);
    checkOnePixelPng(pictureBytes);

    auto const optDescriptor =
      library::test::physicalWriter(musicLibrary.resources(), transaction).get(cover.resourceId);
    REQUIRE(optDescriptor);
    CHECK(optDescriptor->digest == utility::computeSha256(pictureBytes));
    CHECK(optDescriptor->byteLength == pictureBytes.size());
    CHECK(library::deriveResourceId(optDescriptor->digest) == cover.resourceId);
  }

  TEST_CASE("Media File - opus fixture exposes decoded audio properties", "[media][integration][property]")
  {
    auto const* const fixture = GENERATE("basic_metadata.opus", "mono.opus");
    CAPTURE(fixture);

    auto loaded = loadTrack(kTestDataDir / fixture);
    auto& prop = loaded.builder().property();

    CHECK(prop.duration() >= std::chrono::milliseconds{950});
    CHECK(prop.duration() <= std::chrono::milliseconds{1050});

    // Opus always decodes at 48kHz and carries no sample depth of its own.
    CHECK(prop.sampleRate() == 48000);
    CHECK(prop.bitDepth() == 0);
    CHECK(prop.bitrate() > 0);
    CHECK(prop.channels() == (std::string_view{fixture} == "mono.opus" ? 1 : 2));
  }

  // ============================================================================
  // Empty/Missing Metadata Tests
  // ============================================================================
  TEST_CASE("Media File - empty fixture exposes empty metadata", "[media][integration][metadata]")
  {
    auto const* const format = GENERATE("flac", "m4a", "mp3", "wav", "opus");
    auto const path = kTestDataDir / ("empty." + std::string{format});

    auto loaded = loadTrack(path);
    auto& builder = loaded.builder();
    auto& metadata = builder.metadata();
    auto& prop = builder.property();

    // Empty files should still have audio properties
    CHECK(prop.duration() > std::chrono::milliseconds{0});
    CHECK(prop.sampleRate() > 0);
    CHECK(prop.channels() > 0);

    // But metadata should be empty
    CHECK(metadata.title().empty());
    CHECK(metadata.artist().empty());
    CHECK(metadata.album().empty());
    CHECK(metadata.genre().empty());
    CHECK(metadata.trackNumber() == 0);
    CHECK(metadata.year() == 0);
  }
} // namespace ao::media::file::test
