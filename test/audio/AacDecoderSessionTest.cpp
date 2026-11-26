// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/AacDecoderSession.h"

#include "DecoderTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include <ao/AudioCodec.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>

namespace ao::audio::test
{
  TEST_CASE("AacDecoderSession - decodes happy path")
  {
    auto const testFile = requireAudioFixture("basic_metadata.m4a");

    auto decoderPtr = ao::test::requireValue(AacDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();
    CHECK(info.codec == AudioCodec::Aac);
    CHECK(info.duration >= std::chrono::milliseconds{950});
    CHECK(info.sourceFormat.precisionBits == 16);
    CHECK(info.isLossy);

    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK_FALSE(blockRes->bytes.empty());
    CHECK(blockRes->frames > 0);
    CHECK(blockRes->firstFrameIndex == 0);
  }

  TEST_CASE("AacDecoderSession - seeks before the published duration retain the untrimmed final native block")
  {
    auto const testFile = requireAudioFixture("basic_metadata.m4a");

    auto decoderPtr = ao::test::requireValue(AacDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();
    // The last of 45 AAC-LC packets declares just 68 media ticks but decodes
    // to the usual 1024 frames. The published duration is floored to ms.
    REQUIRE(info.duration == std::chrono::milliseconds{1023});
    REQUIRE(info.sourceFormat.sampleRate == 44100);
    REQUIRE(decoder.seek(std::chrono::milliseconds{1022}));
    auto const blockRes = decoder.readNextBlock();

    REQUIRE(blockRes);
    CHECK(blockRes->firstFrameIndex == 45070);
    CHECK(blockRes->frames == 1010);
    CHECK(blockRes->bytes.size() == std::size_t{1010} * 2 * sizeof(std::int16_t));
    CHECK(blockRes->endOfStream);

    for (auto const offset : {info.duration, info.duration + std::chrono::milliseconds{1}})
    {
      REQUIRE(decoder.seek(std::chrono::milliseconds{500}));
      auto const interior = ao::test::requireValue(decoder.readNextBlock());
      REQUIRE(interior.frames > 0);
      REQUIRE_FALSE(interior.endOfStream);
      REQUIRE(decoder.seek(offset));
      auto const eofRes = decoder.readNextBlock();
      REQUIRE(eofRes);
      CHECK(eofRes->endOfStream);
      CHECK(eofRes->frames == 0);
      CHECK(eofRes->bytes.empty());
    }
  }

  TEST_CASE("AacDecoderSession - 32-bit padded output")
  {
    auto const testFile = requireAudioFixture("basic_metadata.m4a");

    auto sourceDecoderPtr = ao::test::requireValue(AacDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& sourceDecoder = *sourceDecoderPtr;
    auto const sourceInfo = sourceDecoder.streamInfo();
    auto const sourceBlockRes = sourceDecoder.readNextBlock();
    REQUIRE(sourceBlockRes);
    REQUIRE(sourceBlockRes->frames > 0);
    REQUIRE(sourceBlockRes->bytes.size() ==
            static_cast<std::size_t>(sourceBlockRes->frames) * sourceInfo.outputFormat.channels * sizeof(std::int16_t));

    auto targetDecoderPtr = ao::test::requireValue(AacDecoderSession::open(testFile, SampleEncoding::Signed32Le));
    auto& targetDecoder = *targetDecoderPtr;

    auto const targetInfo = targetDecoder.streamInfo();
    CHECK(targetInfo.sourceFormat.precisionBits == 16);
    CHECK(encodingContainerBits(targetInfo.outputFormat.encoding) == 32);

    auto const targetBlockRes = targetDecoder.readNextBlock();
    REQUIRE(targetBlockRes);
    REQUIRE(targetBlockRes->frames == sourceBlockRes->frames);
    REQUIRE(targetBlockRes->firstFrameIndex == sourceBlockRes->firstFrameIndex);
    REQUIRE(targetBlockRes->bytes.size() ==
            static_cast<std::size_t>(targetBlockRes->frames) * targetInfo.outputFormat.channels * sizeof(std::int32_t));

    // The first AAC packet may carry encoder priming silence. This comparison
    // locks exact adapter correspondence without claiming nonzero sensitivity.
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

  TEST_CASE("AacDecoderSession - supports lossless output encodings")
  {
    auto const testFile = requireAudioFixture("basic_metadata.m4a");

    for (auto const encoding : {SampleEncoding::Signed24PackedLe,
                                SampleEncoding::Signed24In32Le,
                                SampleEncoding::Signed32Le,
                                SampleEncoding::Float32Le})
    {
      auto decoderPtr = ao::test::requireValue(AacDecoderSession::open(testFile, encoding));
      auto& decoder = *decoderPtr;
      auto const info = decoder.streamInfo();
      CHECK(info.outputFormat.encoding == encoding);

      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);
      REQUIRE(blockRes->frames > 0);
      CHECK(blockRes->firstFrameIndex == 0);
      CHECK(blockRes->bytes.size() ==
            static_cast<std::size_t>(blockRes->frames) * info.outputFormat.channels * bytesPerSample(encoding));
    }
  }

  TEST_CASE("AacDecoderSession - reports error paths")
  {
    SECTION("Non-existent file")
    {
      CHECK(!AacDecoderSession::open("/path/to/nowhere/nonexistent.m4a", SampleEncoding::Signed16Le));
    }

    SECTION("Invalid file content")
    {
      auto const tempFile = ao::test::TempFile{".m4a"};
      {
        auto ofs = std::ofstream{tempFile.path, std::ios::binary};
        ofs << "NOT AN AAC FILE! Random garbage data...";
      }

      CHECK(!AacDecoderSession::open(tempFile.path, SampleEncoding::Signed16Le));
    }
  }

  TEST_CASE("AacDecoderSession - reports end of stream")
  {
    auto const testFile = requireAudioFixture("basic_metadata.m4a");

    auto decoderPtr = ao::test::requireValue(AacDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    CHECK(readUntilStableEndOfStream(decoder, 256) > 0);
  }
} // namespace ao::audio::test
