// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/Mp3DecoderSession.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>

namespace ao::audio::test
{
  TEST_CASE("Mp3DecoderSession - Happy Path", "[audio][unit][mp3]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "hires.mp3";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'hires.mp3' missing");
    }

    auto decoder = Mp3DecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    REQUIRE(info.sourceFormat.sampleRate == 48000);
    REQUIRE(info.sourceFormat.channels == 2);
    REQUIRE(info.outputFormat.sampleRate == info.sourceFormat.sampleRate);
    REQUIRE(info.outputFormat.channels == info.sourceFormat.channels);
    REQUIRE(info.outputFormat.bitDepth == 16);
    REQUIRE(info.durationMs > 0);

    auto const firstBlock = decoder.readNextBlock();
    REQUIRE(firstBlock);
    CHECK(firstBlock->firstFrameIndex == 0);
    CHECK(firstBlock->frames > 0);
    CHECK(!firstBlock->bytes.empty());

    REQUIRE(decoder.seek(500));
    auto const soughtBlock = decoder.readNextBlock();
    REQUIRE(soughtBlock);
    CHECK(soughtBlock->firstFrameIndex > 0);

    decoder.flush();
    CHECK(decoder.readNextBlock());
  }

  TEST_CASE("Mp3DecoderSession - Empty Output Format Probes Native Stream", "[audio][unit][mp3]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.mp3";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'basic_metadata.mp3' missing");
    }

    auto decoder = Mp3DecoderSession{Format{.isInterleaved = true}};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    CHECK(info.sourceFormat.sampleRate == 44100);
    CHECK(info.sourceFormat.channels == 2);
    CHECK(info.sourceFormat.bitDepth == 16);
    CHECK(info.outputFormat == info.sourceFormat);
    CHECK(info.isLossy);

    auto const block = decoder.readNextBlock();
    REQUIRE(block);
    CHECK(block->bitDepth == 16);
    CHECK(block->frames > 0);
    CHECK(block->bytes.size() == static_cast<std::size_t>(block->frames) * 2U * 2U);
  }

  TEST_CASE("Mp3DecoderSession - Floating Point Output", "[audio][unit][mp3]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "hires.mp3";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'hires.mp3' missing");
    }

    // Aobus often uses 32-bit float for internal processing
    auto decoder = Mp3DecoderSession{Format{.bitDepth = 32, .isFloat = true, .isInterleaved = true}};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    CHECK(info.outputFormat.isFloat);
    CHECK(info.outputFormat.bitDepth == 32);

    auto const block = decoder.readNextBlock();
    REQUIRE(block);
    CHECK(block->bitDepth == 32);
    CHECK(block->bytes.size() == static_cast<std::size_t>(block->frames) * 2U * 4U);
  }

  TEST_CASE("Mp3DecoderSession - Re-opening", "[audio][unit][mp3]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "hires.mp3";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'hires.mp3' missing");
    }

    auto decoder = Mp3DecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

    REQUIRE(decoder.open(testFile));
    REQUIRE(decoder.readNextBlock());

    // Open same file again
    REQUIRE(decoder.open(testFile));
    auto const block = decoder.readNextBlock();
    REQUIRE(block);
    CHECK(block->firstFrameIndex == 0); // Should be reset
  }

  TEST_CASE("Mp3DecoderSession - Read Until EOF", "[audio][unit][mp3]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.mp3";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'basic_metadata.mp3' missing");
    }

    auto decoder = Mp3DecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
    REQUIRE(decoder.open(testFile));

    bool eofReached = false;
    std::uint64_t totalFrames = 0;

    while (true)
    {
      auto blockResult = decoder.readNextBlock();
      REQUIRE(blockResult);

      if (blockResult->endOfStream)
      {
        eofReached = true;
        break;
      }

      CHECK(blockResult->frames > 0);
      totalFrames += blockResult->frames;
    }

    CHECK(eofReached);
    CHECK(totalFrames == 44100);

    // Reading again should immediately return endOfStream (covers _implPtr->eof check)
    auto blockAfterEof = decoder.readNextBlock();
    REQUIRE(blockAfterEof);
    CHECK(blockAfterEof->endOfStream);
  }

  TEST_CASE("Mp3DecoderSession - Error Paths", "[audio][unit][mp3][error]")
  {
    auto decoder = Mp3DecoderSession{Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isInterleaved = true}};

    SECTION("Seek on unopened file")
    {
      CHECK(!decoder.seek(100));
    }

    SECTION("Non-existent file")
    {
      CHECK(!decoder.open("/path/to/nowhere/nonexistent.mp3"));
    }

    SECTION("Invalid file content")
    {
      auto const tempFile = std::filesystem::temp_directory_path() / "invalid_mp3.mp3";
      {
        auto ofs = std::ofstream{tempFile, std::ios::binary};
        ofs << "NOT AN MP3 FILE! Random garbage data...";
      }

      CHECK(!decoder.open(tempFile));
      std::filesystem::remove(tempFile);
    }

    SECTION("Seek way beyond duration")
    {
      auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.mp3";

      if (std::filesystem::exists(testFile))
      {
        REQUIRE(decoder.open(testFile));
        // Seek to 1 hour (much longer than basic_metadata.mp3)
        CHECK(!decoder.seek(3600 * 1000));
      }
    }

    SECTION("Unsupported 32-bit integer output")
    {
      auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.mp3";

      if (std::filesystem::exists(testFile))
      {
        auto int32Decoder = Mp3DecoderSession{Format{
          .sampleRate = 44100,
          .channels = 2,
          .bitDepth = 32,
          .isFloat = false,
          .isInterleaved = true,
        }};

        CHECK(!int32Decoder.open(testFile));
      }
    }

    SECTION("Unsupported sample rate conversion")
    {
      auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "hires.mp3";

      if (std::filesystem::exists(testFile))
      {
        auto resamplingDecoder = Mp3DecoderSession{Format{
          .sampleRate = 44100,
          .channels = 2,
          .bitDepth = 16,
          .isInterleaved = true,
        }};

        CHECK(!resamplingDecoder.open(testFile));
      }
    }
  }
} // namespace ao::audio::test
