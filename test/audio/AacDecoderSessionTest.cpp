// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/AacDecoderSession.h"

#include "DecoderTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include <ao/AudioCodec.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_test_macros.hpp>

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

    CHECK(readUntilStableEndOfStream(decoder, 256) == 46080); // 45 full 1024-frame blocks.
  }
} // namespace ao::audio::test
