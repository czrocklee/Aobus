// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/FlacDecoderSession.h"

#include "DecoderTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
    REQUIRE(info.sourceFormat.sampleRate > 0);
    REQUIRE(info.duration > std::chrono::milliseconds{500});

    auto const firstBlockRes = decoder.readNextBlock();
    REQUIRE(firstBlockRes);
    CHECK(firstBlockRes->firstFrameIndex == 0);

    constexpr auto kSeekOffset = std::chrono::milliseconds{500};
    auto const targetFrame = (static_cast<std::uint64_t>(kSeekOffset.count()) * info.sourceFormat.sampleRate) / 1000U;
    REQUIRE(decoder.seek(kSeekOffset));
    auto const soughtBlockRes = decoder.readNextBlock();
    REQUIRE(soughtBlockRes);
    REQUIRE(soughtBlockRes->frames > 0);
    CHECK(soughtBlockRes->firstFrameIndex == targetFrame);
    CHECK(soughtBlockRes->firstFrameIndex + soughtBlockRes->frames > targetFrame);

    auto const frameAfterSoughtBlock = soughtBlockRes->firstFrameIndex + soughtBlockRes->frames;
    decoder.flush();
    auto const flushedBlockRes = decoder.readNextBlock();
    REQUIRE(flushedBlockRes);
    CHECK(flushedBlockRes->frames > 0);
    CHECK(flushedBlockRes->firstFrameIndex == frameAfterSoughtBlock);
  }

  TEST_CASE("FlacDecoderSession - 24-bit", "[audio][unit][flac]")
  {
    auto const testFile = requireAudioFixture("hires.flac");

    auto packedDecoderPtr =
      ao::test::requireValue(FlacDecoderSession::open(testFile, SampleEncoding::Signed24PackedLe));
    auto& packedDecoder = *packedDecoderPtr;
    auto const packedInfo = packedDecoder.streamInfo();
    CHECK(packedInfo.sourceFormat.precisionBits == 24);
    CHECK(packedInfo.outputFormat.encoding == SampleEncoding::Signed24PackedLe);
    auto const packedBlockRes = packedDecoder.readNextBlock();
    REQUIRE(packedBlockRes);
    REQUIRE(packedBlockRes->frames > 0);
    REQUIRE(packedBlockRes->bytes.size() ==
            static_cast<std::size_t>(packedBlockRes->frames) * packedInfo.outputFormat.channels * 3U);

    auto paddedDecoderPtr = ao::test::requireValue(FlacDecoderSession::open(testFile, SampleEncoding::Signed32Le));
    auto& paddedDecoder = *paddedDecoderPtr;
    auto const paddedInfo = paddedDecoder.streamInfo();
    CHECK(paddedInfo.outputFormat.encoding == SampleEncoding::Signed32Le);
    auto const paddedBlockRes = paddedDecoder.readNextBlock();
    REQUIRE(paddedBlockRes);
    REQUIRE(paddedBlockRes->frames == packedBlockRes->frames);
    REQUIRE(paddedBlockRes->firstFrameIndex == packedBlockRes->firstFrameIndex);
    REQUIRE(paddedBlockRes->bytes.size() ==
            static_cast<std::size_t>(paddedBlockRes->frames) * paddedInfo.outputFormat.channels * sizeof(std::int32_t));

    auto const packedSamples = packedBlockRes->bytes.size() / 3U;
    auto const paddedSamples = paddedBlockRes->bytes.size() / sizeof(std::int32_t);
    auto const samplesToCheck = std::min({packedSamples, paddedSamples, std::size_t{128}});
    REQUIRE(samplesToCheck > 0);

    for (std::size_t index = 0; index < samplesToCheck; ++index)
    {
      auto const packedSample = readSigned24PackedLePcmSample(packedBlockRes->bytes, index);
      auto const paddedSample = readSigned32LePcmSample(paddedBlockRes->bytes, index);
      CHECK(paddedSample == packedSample * 256);
    }
  }

  TEST_CASE("FlacDecoderSession - rejects precision-losing output", "[audio][unit][flac]")
  {
    auto const testFile = requireAudioFixture("hires.flac");
    auto const res = FlacDecoderSession::open(testFile, SampleEncoding::Signed16Le);
    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::NotSupported);
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
    REQUIRE(info.sourceFormat.sampleRate > 0);
    auto const durationFrame =
      (static_cast<std::uint64_t>(info.duration.count()) * info.sourceFormat.sampleRate) / 1000U;
    REQUIRE(durationFrame > 0);
    auto const finalFrame = durationFrame - 1U;

    REQUIRE(decoder.seek(info.duration));
    auto const finalBlockRes = decoder.readNextBlock();
    REQUIRE(finalBlockRes);
    REQUIRE(finalBlockRes->frames > 0);
    CHECK(finalBlockRes->firstFrameIndex == finalFrame);
    CHECK(finalBlockRes->firstFrameIndex + finalBlockRes->frames > finalFrame);
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
