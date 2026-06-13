// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "DecoderTestUtils.h"
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/FlacDecoderSession.h>
#include <ao/audio/Format.h>
#include <ao/audio/PcmConverter.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("FlacDecoderSession - Happy Path", "[audio][unit][flac]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "hires.flac";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'hires.flac' missing");
    }

    auto decoder = FlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    REQUIRE(info.sourceFormat.sampleRate > 0);
    REQUIRE(info.duration > std::chrono::milliseconds{0});

    auto const firstBlock = decoder.readNextBlock();
    REQUIRE(firstBlock);
    CHECK(firstBlock->firstFrameIndex == 0);

    REQUIRE(decoder.seek(std::chrono::milliseconds{500}));
    auto const soughtBlock = decoder.readNextBlock();
    REQUIRE(soughtBlock);
    CHECK(soughtBlock->firstFrameIndex > 0);

    decoder.flush();
    CHECK(decoder.readNextBlock()); // Should read again from where we were, or next block
  }

  TEST_CASE("FlacDecoderSession - 24-bit", "[audio][unit][flac]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "hires.flac";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'hires.flac' missing");
    }

    auto decoder = FlacDecoderSession{Format{.bitDepth = 24, .isInterleaved = true}};
    REQUIRE(decoder.open(testFile));
    REQUIRE(decoder.readNextBlock());

    auto paddedDecoder = FlacDecoderSession{Format{.bitDepth = 32, .isInterleaved = true}};
    REQUIRE(paddedDecoder.open(testFile));
    CHECK(paddedDecoder.streamInfo().outputFormat.bitDepth == 32);
    CHECK(paddedDecoder.streamInfo().outputFormat.validBits == 24);
    auto const block = paddedDecoder.readNextBlock();
    REQUIRE(block);
    CHECK(block->bitDepth == 32);
    CHECK_FALSE(block->bytes.empty());
  }

  TEST_CASE("FlacDecoderSession - scales 24-bit samples for 16-bit output", "[audio][unit][flac]")
  {
    auto const testFile = requireAudioFixture("hires.flac");
    auto sourceDecoder = FlacDecoderSession{Format{.bitDepth = 24, .isInterleaved = true}};
    auto targetDecoder = FlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

    REQUIRE(sourceDecoder.open(testFile));
    REQUIRE(targetDecoder.open(testFile));

    auto const sourceBlock = sourceDecoder.readNextBlock();
    auto const targetBlock = targetDecoder.readNextBlock();
    REQUIRE(sourceBlock);
    REQUIRE(targetBlock);
    REQUIRE(sourceBlock->frames == targetBlock->frames);

    auto sourceSamples = std::vector<std::int32_t>(sourceBlock->bytes.size() / 3U);
    PcmConverter::unpackS24(sourceBlock->bytes, sourceSamples);
    auto const targetSamples = utility::layout::viewArray<std::int16_t>(targetBlock->bytes);
    auto const samplesToCheck = std::min({sourceSamples.size(), targetSamples.size(), std::size_t{256}});
    REQUIRE(samplesToCheck > 0);

    for (std::size_t index = 0; index < samplesToCheck; ++index)
    {
      CHECK(targetSamples[index] == static_cast<std::int16_t>(sourceSamples[index] >> 8U));
    }
  }

  TEST_CASE("FlacDecoderSession - stable end of stream", "[audio][unit][flac]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.flac");
    auto decoder = FlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

    REQUIRE(decoder.open(testFile));
    CHECK(readUntilStableEndOfStream(decoder, 512) > 0);
  }

  TEST_CASE("FlacDecoderSession - Error Paths", "[audio][unit][flac][error]")
  {
    auto decoder = FlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

    SECTION("Seek on unopened file")
    {
      CHECK(!decoder.seek(std::chrono::milliseconds{100})); // Should fail gracefully
    }

    SECTION("Non-existent file")
    {
      CHECK(!decoder.open("/path/to/nowhere/nonexistent.flac"));
    }

    SECTION("Invalid file content")
    {
      auto const tempFile = std::filesystem::temp_directory_path() / "invalid_flac.flac";
      {
        auto ofs = std::ofstream{tempFile, std::ios::binary};
        ofs << "NOT A FLAC FILE! Random garbage data...";
      }

      CHECK(!decoder.open(tempFile));
      std::filesystem::remove(tempFile);
    }

    SECTION("Unsupported fixed output requests fail during open")
    {
      auto const testFile = requireAudioFixture("hires.flac");

      CHECK((!FlacDecoderSession{Format{.sampleRate = 1, .bitDepth = 16, .isInterleaved = true}}.open(testFile)));
      CHECK((!FlacDecoderSession{Format{.channels = 1, .bitDepth = 16, .isInterleaved = true}}.open(testFile)));
      CHECK((!FlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = false}}.open(testFile)));
      CHECK((!FlacDecoderSession{Format{.bitDepth = 32, .isFloat = true, .isInterleaved = true}}.open(testFile)));
      CHECK((!FlacDecoderSession{Format{.bitDepth = 32, .validBits = 16, .isInterleaved = true}}.open(testFile)));
      CHECK((!FlacDecoderSession{Format{.bitDepth = 8, .isInterleaved = true}}.open(testFile)));
    }

    SECTION("Close and failed reopen clear stream state")
    {
      auto const testFile = requireAudioFixture("hires.flac");

      REQUIRE(decoder.open(testFile));
      decoder.close();
      decoder.close();
      checkClosedSession(decoder);

      REQUIRE(decoder.open(testFile));
      CHECK(!decoder.open("/path/to/nowhere/nonexistent.flac"));
      checkClosedSession(decoder);
    }
  }
} // namespace ao::audio::test
