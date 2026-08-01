// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/FlacDecoderSession.h>

#include "DecoderTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <ios>

namespace ao::audio::test
{
  TEST_CASE("FlacDecoderSession - decodes happy path", "[audio][unit][flac]")
  {
    auto const testFile = requireAudioFixture("hires.flac");

    auto decoder = FlacDecoderSession{SampleEncoding::Signed24PackedLe};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    CHECK(info.sourceFormat.sampleRate > 0);
    CHECK(info.duration > std::chrono::milliseconds{0});

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
    auto const testFile = requireAudioFixture("hires.flac");

    auto decoder = FlacDecoderSession{SampleEncoding::Signed24PackedLe};
    REQUIRE(decoder.open(testFile));
    CHECK(decoder.readNextBlock());

    auto paddedDecoder = FlacDecoderSession{SampleEncoding::Signed32Le};
    REQUIRE(paddedDecoder.open(testFile));
    CHECK(encodingContainerBits(paddedDecoder.streamInfo().outputFormat.encoding) == 32);
    auto const block = paddedDecoder.readNextBlock();
    REQUIRE(block);
    CHECK_FALSE(block->bytes.empty());
  }

  TEST_CASE("FlacDecoderSession - rejects precision-losing output", "[audio][unit][flac]")
  {
    auto const testFile = requireAudioFixture("hires.flac");
    auto decoder = FlacDecoderSession{SampleEncoding::Signed16Le};

    auto const result = decoder.open(testFile);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotSupported);
  }

  TEST_CASE("FlacDecoderSession - seek to reported duration lands on the final frame", "[audio][unit][flac]")
  {
    // Seeking to the exact reported duration resolves to total_samples, one past
    // the last valid sample. The session must clamp to the last decodable sample
    // so the seek lands on the final frame rather than at end of stream; libFLAC
    // builds otherwise disagree on out-of-range seeks and drop the position.
    auto const testFile = requireAudioFixture("basic_metadata.flac");
    auto decoder = FlacDecoderSession{SampleEncoding::Signed16Le};

    REQUIRE(decoder.open(testFile));
    auto const info = decoder.streamInfo();
    REQUIRE(info.duration > std::chrono::milliseconds{0});

    REQUIRE(decoder.seek(info.duration));
    auto const finalBlock = decoder.readNextBlock();
    REQUIRE(finalBlock);
    CHECK(finalBlock->frames > 0);
  }

  TEST_CASE("FlacDecoderSession - stable end of stream", "[audio][unit][flac]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.flac");
    auto decoder = FlacDecoderSession{SampleEncoding::Signed16Le};

    REQUIRE(decoder.open(testFile));
    CHECK(readUntilStableEndOfStream(decoder, 512) > 0);
  }

  TEST_CASE("FlacDecoderSession - reports error paths", "[audio][unit][flac][error]")
  {
    auto decoder = FlacDecoderSession{SampleEncoding::Signed16Le};

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
      auto const tempFile = ao::test::TempFile{".flac"};
      {
        auto ofs = std::ofstream{tempFile.path, std::ios::binary};
        ofs << "NOT A FLAC FILE! Random garbage data...";
      }

      CHECK(!decoder.open(tempFile.path));
    }

    SECTION("Precision-losing output fails during open")
    {
      auto const testFile = requireAudioFixture("hires.flac");

      CHECK(!FlacDecoderSession{SampleEncoding::Signed16Le}.open(testFile));
      CHECK(FlacDecoderSession{SampleEncoding::Float32Le}.open(testFile));
      CHECK(FlacDecoderSession{SampleEncoding::Signed32Le}.open(testFile));
    }

    SECTION("Close and failed reopen clear stream state")
    {
      auto const testFile = requireAudioFixture("hires.flac");
      auto lifecycleDecoder = FlacDecoderSession{SampleEncoding::Signed24PackedLe};

      REQUIRE(lifecycleDecoder.open(testFile));
      lifecycleDecoder.close();
      lifecycleDecoder.close();
      checkClosedSession(lifecycleDecoder);

      REQUIRE(lifecycleDecoder.open(testFile));
      CHECK(!lifecycleDecoder.open("/path/to/nowhere/nonexistent.flac"));
      checkClosedSession(lifecycleDecoder);
    }
  }
} // namespace ao::audio::test
