// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/AlacDecoderSession.h"

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
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("AlacDecoderSession - seek", "[audio][unit][alac][seek]")
  {
    auto const testFile = requireAudioFixture("hires.m4a");

    auto decoderPtr = ao::test::requireValue(AlacDecoderSession::open(testFile, SampleEncoding::Signed24PackedLe));
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
    CHECK(soughtBlockRes->firstFrameIndex > 0);
    CHECK(soughtBlockRes->firstFrameIndex <= targetFrame);
    CHECK(soughtBlockRes->firstFrameIndex + soughtBlockRes->frames > targetFrame);

    REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
    auto const resetBlockRes = decoder.readNextBlock();

    REQUIRE(resetBlockRes);
    CHECK(resetBlockRes->firstFrameIndex == 0);

    decoder.flush();
    CHECK(decoder.readNextBlock());
  }

  TEST_CASE("AlacDecoderSession - output formats", "[audio][unit][alac]")
  {
    SECTION("Decodes 24-bit ALAC into 32-bit output")
    {
      auto const testFile = requireAudioFixture("hires.m4a");

      auto packedDecoderPtr =
        ao::test::requireValue(AlacDecoderSession::open(testFile, SampleEncoding::Signed24PackedLe));
      auto& packedDecoder = *packedDecoderPtr;
      auto const packedInfo = packedDecoder.streamInfo();
      CHECK(packedInfo.sourceFormat.precisionBits == 24);
      CHECK(packedInfo.outputFormat.encoding == SampleEncoding::Signed24PackedLe);

      auto const packedBlockRes = packedDecoder.readNextBlock();
      REQUIRE(packedBlockRes);
      REQUIRE(packedBlockRes->frames > 0);
      REQUIRE(packedBlockRes->bytes.size() ==
              static_cast<std::size_t>(packedBlockRes->frames) * packedInfo.outputFormat.channels * 3U);

      auto paddedDecoderPtr = ao::test::requireValue(AlacDecoderSession::open(testFile, SampleEncoding::Signed32Le));
      auto& paddedDecoder = *paddedDecoderPtr;
      auto const paddedInfo = paddedDecoder.streamInfo();
      CHECK(paddedInfo.sourceFormat.precisionBits == 24);
      CHECK(paddedInfo.outputFormat.encoding == SampleEncoding::Signed32Le);

      auto const paddedBlockRes = paddedDecoder.readNextBlock();
      REQUIRE(paddedBlockRes);
      REQUIRE(paddedBlockRes->frames == packedBlockRes->frames);
      REQUIRE(paddedBlockRes->firstFrameIndex == packedBlockRes->firstFrameIndex);
      REQUIRE(paddedBlockRes->bytes.size() == static_cast<std::size_t>(paddedBlockRes->frames) *
                                                paddedInfo.outputFormat.channels * sizeof(std::int32_t));

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

    SECTION("Pads 16-bit ALAC samples into 32-bit output")
    {
      auto const testFile = requireAudioFixture("alac16.m4a");

      auto sourceDecoderPtr = ao::test::requireValue(AlacDecoderSession::open(testFile, SampleEncoding::Signed16Le));
      auto& sourceDecoder = *sourceDecoderPtr;

      auto const sourceInfo = sourceDecoder.streamInfo();
      CHECK(sourceInfo.sourceFormat.sampleRate == 44100);
      CHECK(sourceInfo.sourceFormat.channels == 2);
      CHECK(sourceInfo.sourceFormat.precisionBits == 16);

      auto const sourceBlockRes = sourceDecoder.readNextBlock();
      REQUIRE(sourceBlockRes);
      REQUIRE(!sourceBlockRes->bytes.empty());

      auto targetDecoderPtr = ao::test::requireValue(AlacDecoderSession::open(testFile, SampleEncoding::Signed32Le));
      auto& targetDecoder = *targetDecoderPtr;

      auto const targetInfo = targetDecoder.streamInfo();
      CHECK(encodingContainerBits(targetInfo.outputFormat.encoding) == 32);

      auto const targetBlockRes = targetDecoder.readNextBlock();
      REQUIRE(targetBlockRes);
      REQUIRE(!targetBlockRes->bytes.empty());

      auto const sourceSamples = sourceBlockRes->bytes.size() / sizeof(std::int16_t);
      auto const targetSamples = targetBlockRes->bytes.size() / sizeof(std::int32_t);
      auto const samplesToCheck = std::min({sourceSamples, targetSamples, std::size_t{128}});

      REQUIRE(samplesToCheck > 0);

      for (std::size_t index = 0; index < samplesToCheck; ++index)
      {
        auto const sourceSample = readSigned16LePcmSample(sourceBlockRes->bytes, index);
        auto const targetSample = readSigned32LePcmSample(targetBlockRes->bytes, index);
        CHECK(targetSample == static_cast<std::int32_t>(sourceSample) * 65536);
      }
    }
  }

  TEST_CASE("AlacDecoderSession - reading through the final packet reaches stable end of stream", "[audio][unit][alac]")
  {
    auto const testFile = requireAudioFixture("hires.m4a");
    auto decoderPtr = ao::test::requireValue(AlacDecoderSession::open(testFile, SampleEncoding::Signed24PackedLe));
    auto& decoder = *decoderPtr;

    CHECK(readUntilStableEndOfStream(decoder, 512) > 0);
  }

  TEST_CASE("AlacDecoderSession - rejects invalid input", "[audio][unit][alac][error]")
  {
    SECTION("Non-existent file")
    {
      CHECK(!AlacDecoderSession::open("/path/to/nowhere/nonexistent.m4a", SampleEncoding::Signed16Le));
    }

    SECTION("Non-MP4 content")
    {
      auto const garbage = std::vector<std::uint8_t>{'N', 'O', 'T', ' ', 'M', 'P', '4'};
      auto const temp = ao::test::TempFile{garbage, ".m4a"};
      CHECK(!AlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
    }

    SECTION("Unsupported 24-bit to 16-bit conversion")
    {
      auto const testFile = requireAudioFixture("hires.m4a");

      CHECK(!AlacDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    }

    SECTION("Lossless fixed output requests succeed during open")
    {
      auto const testFile = requireAudioFixture("alac16.m4a");

      CHECK(AlacDecoderSession::open(testFile, SampleEncoding::Float32Le));
      CHECK(AlacDecoderSession::open(testFile, SampleEncoding::Signed24In32Le));
    }
  }
} // namespace ao::audio::test
