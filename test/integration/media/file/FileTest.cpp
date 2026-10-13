// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/library/MediaTrack.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/PictureType.h>
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceLayout.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackView.h>
#include <ao/media/flac/MetadataBlockLayout.h>
#include <ao/utility/Sha256.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

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

    std::vector<library::Credit> copyCredits(std::span<library::CreditView const> entries)
    {
      auto owned = std::vector<library::Credit>{};
      owned.reserve(entries.size());

      for (auto const& entry : entries)
      {
        owned.push_back({.name = std::string{entry.name}, .kind = entry.kind, .role = std::string{entry.role}});
      }

      return owned;
    }

    std::vector<std::uint8_t> makeAllKindCreditFlac()
    {
      auto bytes = std::vector<std::uint8_t>{'f', 'L', 'a', 'C', 0, 0, 0, 34};
      auto streamInfo = flac::StreamInfoLayout{};
      streamInfo.packedFields = (44100ULL << 44) | (1ULL << 41) | (15ULL << 36) | 44100ULL;
      auto const* streamBytes = reinterpret_cast<std::uint8_t const*>(&streamInfo);
      bytes.insert(bytes.end(), streamBytes, streamBytes + sizeof(streamInfo));
      auto comments = std::vector<std::uint8_t>{};
      auto const appendSize = [&](std::size_t size)
      {
        for (auto const shift : {0U, 8U, 16U, 24U})
        {
          comments.push_back(static_cast<std::uint8_t>(size >> shift));
        }
      };
      auto const values = std::to_array<std::string_view>({
        "SOLOIST=Solo",
        "PERFORMER=Ada (Piano)",
        "ORCHESTRA=Discarded",
        "CONDUCTOR=First",
        "ENSEMBLE=Group",
        "CONDUCTOR=Second",
        "PERFORMER=Ada (Piano)",
        "CONDUCTOR=First",
        "DATE=2024-05-17",
      });
      appendSize(0); // Empty vendor.
      appendSize(values.size());

      for (auto const value : values)
      {
        appendSize(value.size());
        comments.insert(comments.end(), value.begin(), value.end());
      }

      bytes.insert(bytes.end(),
                   {0x84,
                    static_cast<std::uint8_t>(comments.size() >> 16U),
                    static_cast<std::uint8_t>(comments.size() >> 8U),
                    static_cast<std::uint8_t>(comments.size())});
      bytes.insert(bytes.end(), comments.begin(), comments.end());
      bytes.push_back(0xA0); // Nonempty audio payload; no decoder is needed for metadata extraction.
      return bytes;
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
    CHECK_FALSE(metadata.recordingDate().isPresent());

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
    CHECK_FALSE(metadata.recordingDate().isPresent());

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
    CHECK(copyCredits(metadata.credits()) == std::vector<library::Credit>{
                                               {.name = "Fixture Conductor", .kind = library::CreditKind::Conductor},
                                               {.name = "Fixture Ensemble", .kind = library::CreditKind::Ensemble},
                                               {.name = "Fixture Soloist", .kind = library::CreditKind::Soloist},
                                             });
    CHECK_FALSE(metadata.recordingDate().isPresent());
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

    // Copy every borrowed collector view out before the file dies.
    auto const observed = [path]
    {
      auto loaded = loadTrack(path);
      auto const& metadata = loaded.builder().metadata();
      CHECK_FALSE(metadata.recordingDate().isPresent());
      return std::tuple{std::string{metadata.title()}, copyCredits(metadata.credits())};
    }();
    auto const& [title, credits] = observed;

    CHECK(title == "Classical Fallback");

    // PERFORMER is not Soloist; Ensemble itself is also a credit.
    if (std::string_view{format} == "flac" || std::string_view{format} == "opus")
    {
      CHECK(credits == std::vector<library::Credit>{
                         {.name = "Fixture Fallback Ensemble", .kind = library::CreditKind::Ensemble},
                         {.name = "Fixture Fallback Soloist", .kind = library::CreditKind::Performer},
                       });
    }
    else
    {
      CHECK(credits ==
            std::vector<library::Credit>{{.name = "Fixture Fallback Ensemble", .kind = library::CreditKind::Ensemble}});
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
    CHECK(metadata.credits().empty());
    CHECK_FALSE(metadata.recordingDate().isPresent());
  }

  TEST_CASE("Media File - all credit kinds map explicitly and owning copies prepare in canonical library order",
            "[media][integration][metadata]")
  {
    auto const source = []
    {
      auto const temp = ao::test::TempFile{makeAllKindCreditFlac(), ".flac"};
      auto loaded = loadTrack(temp.path);
      CHECK(loaded.builder().metadata().year() == 2024);
      CHECK_FALSE(loaded.builder().metadata().recordingDate().isPresent());
      auto const credits = copyCredits(loaded.builder().metadata().credits());
      CHECK(credits == std::vector<library::Credit>{
                         {.name = "Solo", .kind = library::CreditKind::Soloist},
                         {.name = "Ada", .kind = library::CreditKind::Performer, .role = "Piano"},
                         {.name = "First", .kind = library::CreditKind::Conductor},
                         {.name = "Group", .kind = library::CreditKind::Ensemble},
                         {.name = "Second", .kind = library::CreditKind::Conductor},
                         {.name = "Ada", .kind = library::CreditKind::Performer, .role = "Piano"},
                         {.name = "First", .kind = library::CreditKind::Conductor},
                       });
      return credits;
    }();

    // The MediaTrack/file and callback strings have expired. These owning facts
    // remain alive until builder preparation has copied the necessary IDs.
    auto builder = library::TrackBuilder::makeEmpty();
    builder.property().uri("credits.flac");
    builder.metadata().year(2024).credits(std::span<library::Credit const>{source});
    auto const tempDir = ao::test::TempDir{};
    auto musicLibrary = library::test::makeTestMusicLibrary(tempDir.path(), tempDir.path() / "db");
    auto transaction = library::test::writeTransaction(musicLibrary);
    auto serializeRes = library::test::physicalSerializeTrack(builder, transaction, musicLibrary.resources());
    REQUIRE(serializeRes);
    REQUIRE(transaction.commit());
    auto const& [hot, cold] = *serializeRes;
    auto const view = library::TrackView{hot, cold};
    CHECK(view.metadata().year() == 2024);
    CHECK_FALSE(view.performance().recordingDate().isPresent());
    auto const entries = view.performance().credits();
    REQUIRE(entries.size() == 7);
    auto const& dictionary = musicLibrary.dictionary();
    CHECK(dictionary.get(entries[0].nameId) == "First");
    CHECK(dictionary.get(entries[1].nameId) == "Second");
    CHECK(dictionary.get(entries[2].nameId) == "First");
    CHECK(dictionary.get(entries[3].nameId) == "Group");
    CHECK(dictionary.get(entries[4].nameId) == "Solo");
    CHECK(dictionary.get(entries[5].nameId) == "Ada");
    CHECK(dictionary.get(entries[5].roleId) == "Piano");
    CHECK(entries[6].nameId == entries[5].nameId);
    CHECK(entries[6].roleId == entries[5].roleId);
    CHECK(view.performance().credits(library::CreditKind::Conductor).size() == 3);
    CHECK(view.performance().credits(library::CreditKind::Ensemble).size() == 1);
    CHECK(view.performance().credits(library::CreditKind::Soloist).size() == 1);
    CHECK(view.performance().credits(library::CreditKind::Performer).size() == 2);

    builder.metadata().credits(std::span<library::Credit const>{});
    CHECK(builder.metadata().credits().empty());
    CHECK(builder.metadata().year() == 2024);
    auto clearTransaction = library::test::writeTransaction(musicLibrary);
    auto clearRes = library::test::physicalSerializeTrack(builder, clearTransaction, musicLibrary.resources());
    REQUIRE(clearRes);
    auto const& [clearedHot, clearedCold] = *clearRes;
    auto const cleared = library::TrackView{clearedHot, clearedCold};
    CHECK(cleared.performance().empty());
    CHECK(cleared.metadata().year() == 2024);
    CHECK(cleared.property().uri() == "credits.flac");
  }
} // namespace ao::media::file::test
