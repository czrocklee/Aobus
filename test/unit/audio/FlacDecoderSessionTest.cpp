// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/FlacDecoderSession.h"

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

    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(testFile, SampleEncoding::Signed24PackedLe));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();
    CHECK(info.sourceFormat.sampleRate > 0);
    CHECK(info.duration > std::chrono::milliseconds{0});

    auto const firstBlockRes = decoder.readNextBlock();
    REQUIRE(firstBlockRes);
    CHECK(firstBlockRes->firstFrameIndex == 0);

    REQUIRE(decoder.seek(std::chrono::milliseconds{500}));
    auto const soughtBlockRes = decoder.readNextBlock();
    REQUIRE(soughtBlockRes);
    CHECK(soughtBlockRes->firstFrameIndex > 0);

    decoder.flush();
    CHECK(decoder.readNextBlock()); // Should read again from where we were, or next block
  }

  TEST_CASE("FlacDecoderSession - 24-bit", "[audio][unit][flac]")
  {
    auto const testFile = requireAudioFixture("hires.flac");

    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(testFile, SampleEncoding::Signed24PackedLe));
    auto& decoder = *decoderPtr;
    CHECK(decoder.readNextBlock());

    auto paddedDecoderPtr = ao::test::requireValue(FlacDecoderSession::open(testFile, SampleEncoding::Signed32Le));
    auto& paddedDecoder = *paddedDecoderPtr;
    CHECK(encodingContainerBits(paddedDecoder.streamInfo().outputFormat.encoding) == 32);
    auto const blockRes = paddedDecoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK_FALSE(blockRes->bytes.empty());
  }

  TEST_CASE("FlacDecoderSession - rejects precision-losing output", "[audio][unit][flac]")
  {
    auto const testFile = requireAudioFixture("hires.flac");
    auto const result = FlacDecoderSession::open(testFile, SampleEncoding::Signed16Le);
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
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;
    auto const info = decoder.streamInfo();
    REQUIRE(info.duration > std::chrono::milliseconds{0});

    REQUIRE(decoder.seek(info.duration));
    auto const finalBlockRes = decoder.readNextBlock();
    REQUIRE(finalBlockRes);
    CHECK(finalBlockRes->frames > 0);
  }

  TEST_CASE("FlacDecoderSession - stable end of stream", "[audio][unit][flac]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.flac");
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;
    CHECK(readUntilStableEndOfStream(decoder, 512) > 0);
  }

  TEST_CASE("FlacDecoderSession - reports error paths", "[audio][unit][flac][error]")
  {
    SECTION("Non-existent file")
    {
      CHECK(!FlacDecoderSession::open("/path/to/nowhere/nonexistent.flac", SampleEncoding::Signed16Le));
    }

    SECTION("Invalid file content")
    {
      auto const tempFile = ao::test::TempFile{".flac"};
      {
        auto ofs = std::ofstream{tempFile.path, std::ios::binary};
        ofs << "NOT A FLAC FILE! Random garbage data...";
      }

      CHECK(!FlacDecoderSession::open(tempFile.path, SampleEncoding::Signed16Le));
    }

    SECTION("Precision-losing output fails during open")
    {
      auto const testFile = requireAudioFixture("hires.flac");

      CHECK(!FlacDecoderSession::open(testFile, SampleEncoding::Signed16Le));
      CHECK(FlacDecoderSession::open(testFile, SampleEncoding::Float32Le));
      CHECK(FlacDecoderSession::open(testFile, SampleEncoding::Signed32Le));
    }
  }
} // namespace ao::audio::test
