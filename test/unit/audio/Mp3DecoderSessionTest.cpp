// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "DecoderTestUtils.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureUtils.h"
#include <ao/AudioCodec.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>
#include <ao/audio/Mp3DecoderSession.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("Mp3DecoderSession - Happy Path", "[audio][unit][mp3]")
  {
    auto const testFile = requireAudioFixture("hires.mp3");

    auto decoder = Mp3DecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    CHECK(info.codec == AudioCodec::Mp3);
    CHECK(info.sourceFormat.sampleRate == 48000);
    CHECK(info.sourceFormat.channels == 2);
    CHECK(info.outputFormat.sampleRate == info.sourceFormat.sampleRate);
    CHECK(info.outputFormat.channels == info.sourceFormat.channels);
    CHECK(info.outputFormat.bitDepth == 16);
    CHECK(info.duration > std::chrono::milliseconds{0});

    auto const firstBlock = decoder.readNextBlock();
    REQUIRE(firstBlock);
    CHECK(firstBlock->firstFrameIndex == 0);
    CHECK(firstBlock->frames > 0);
    CHECK(!firstBlock->bytes.empty());

    REQUIRE(decoder.seek(std::chrono::milliseconds{500}));
    auto const soughtBlock = decoder.readNextBlock();
    REQUIRE(soughtBlock);
    CHECK(soughtBlock->firstFrameIndex > 0);

    decoder.flush();
    CHECK(decoder.readNextBlock());
  }

  TEST_CASE("Mp3DecoderSession - Empty Output Format Probes Native Stream", "[audio][unit][mp3]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.mp3");

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
    auto const testFile = requireAudioFixture("hires.mp3");

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
    auto const testFile = requireAudioFixture("hires.mp3");

    auto decoder = Mp3DecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

    REQUIRE(decoder.open(testFile));
    CHECK(decoder.readNextBlock());

    // Open same file again
    REQUIRE(decoder.open(testFile));
    auto const block = decoder.readNextBlock();
    REQUIRE(block);
    CHECK(block->firstFrameIndex == 0); // Should be reset
  }

  TEST_CASE("Mp3DecoderSession - Read Until EOF", "[audio][unit][mp3]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.mp3");

    auto decoder = Mp3DecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
    REQUIRE(decoder.open(testFile));

    CHECK(readUntilStableEndOfStream(decoder, 512) == 44100);
  }

  TEST_CASE("Mp3DecoderSession - rejects a midstream format change", "[audio][unit][mp3][error]")
  {
    auto const firstFile = requireAudioFixture("basic_metadata.mp3");
    auto const secondFile = requireAudioFixture("hires.mp3");
    auto data = readFileBytes(firstFile);
    auto const secondData = readFileBytes(secondFile);
    data.insert(data.end(), secondData.begin(), secondData.end());

    auto const temp = ao::test::TempFile{data, ".mp3"};
    auto decoder = Mp3DecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
    REQUIRE(decoder.open(temp.path));
    auto const initialInfo = decoder.streamInfo();

    bool rejectedFormatChange = false;

    for (std::int32_t count = 0; count < 512 && !rejectedFormatChange; ++count)
    {
      if (auto const block = decoder.readNextBlock(); !block)
      {
        CHECK(block.error().code == Error::Code::NotSupported);
        rejectedFormatChange = true;
      }
      else if (block->endOfStream)
      {
        break;
      }
    }

    CHECK(rejectedFormatChange);
    CHECK(decoder.streamInfo().outputFormat == initialInfo.outputFormat);

    auto const repeatedRead = decoder.readNextBlock();
    REQUIRE_FALSE(repeatedRead);
    CHECK(repeatedRead.error().code == Error::Code::NotSupported);

    REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
    auto const recoveredBlock = decoder.readNextBlock();
    REQUIRE(recoveredBlock);
    CHECK(recoveredBlock->frames > 0);
  }

  TEST_CASE("Mp3DecoderSession - Error Paths", "[audio][unit][mp3][error]")
  {
    auto decoder = Mp3DecoderSession{Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isInterleaved = true}};

    SECTION("Seek on unopened file")
    {
      CHECK(!decoder.seek(std::chrono::milliseconds{100}));
    }

    SECTION("Non-existent file")
    {
      CHECK(!decoder.open("/path/to/nowhere/nonexistent.mp3"));
    }

    SECTION("Invalid file content")
    {
      auto const tempFile = ao::test::TempFile{".mp3"};
      {
        auto ofs = std::ofstream{tempFile.path, std::ios::binary};
        ofs << "NOT AN MP3 FILE! Random garbage data...";
      }

      auto const result = decoder.open(tempFile.path);
      REQUIRE_FALSE(result);
      CHECK(result.error().message.contains(":"));
      CHECK(result.error().message != "Failed to get MP3 format: A generic mpg123 error.");
    }

    SECTION("Seek way beyond duration")
    {
      auto const testFile = requireAudioFixture("basic_metadata.mp3");
      REQUIRE(decoder.open(testFile));
      // Seek to 1 hour (much longer than basic_metadata.mp3)
      CHECK(!decoder.seek(std::chrono::hours{1}));
    }

    SECTION("Unsupported 32-bit integer output")
    {
      auto const testFile = requireAudioFixture("basic_metadata.mp3");
      auto int32Decoder = Mp3DecoderSession{Format{
        .sampleRate = 44100,
        .channels = 2,
        .bitDepth = 32,
        .isFloat = false,
        .isInterleaved = true,
      }};

      CHECK(!int32Decoder.open(testFile));
    }

    SECTION("Unsupported sample rate conversion")
    {
      auto const testFile = requireAudioFixture("hires.mp3");
      auto resamplingDecoder = Mp3DecoderSession{Format{
        .sampleRate = 44100,
        .channels = 2,
        .bitDepth = 16,
        .isInterleaved = true,
      }};

      CHECK(!resamplingDecoder.open(testFile));
    }

    SECTION("Rejects planar output and channel remapping")
    {
      auto const testFile = requireAudioFixture("basic_metadata.mp3");

      CHECK((!Mp3DecoderSession{Format{.bitDepth = 16, .isInterleaved = false}}.open(testFile)));
      CHECK((!Mp3DecoderSession{Format{.channels = 1, .bitDepth = 16, .isInterleaved = true}}.open(testFile)));
      CHECK((!Mp3DecoderSession{Format{.bitDepth = 16, .validBits = 8, .isInterleaved = true}}.open(testFile)));
      CHECK((!Mp3DecoderSession{Format{.channels = 3, .bitDepth = 16, .isInterleaved = true}}.open(testFile)));
      CHECK((!Mp3DecoderSession{Format{.bitDepth = 64, .isFloat = true, .isInterleaved = true}}.open(testFile)));
    }

    SECTION("Close and failed reopen clear stream state")
    {
      auto const testFile = requireAudioFixture("basic_metadata.mp3");
      auto lifecycleDecoder = Mp3DecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      REQUIRE(lifecycleDecoder.open(testFile));
      lifecycleDecoder.close();
      lifecycleDecoder.close();
      checkClosedSession(lifecycleDecoder);

      REQUIRE(lifecycleDecoder.open(testFile));
      CHECK(!lifecycleDecoder.open("/path/to/nowhere/nonexistent.mp3"));
      checkClosedSession(lifecycleDecoder);
    }
  }
} // namespace ao::audio::test
