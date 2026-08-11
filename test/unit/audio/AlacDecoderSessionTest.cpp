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

    auto decoder = AlacDecoderSession{SampleEncoding::Signed24PackedLe};
    REQUIRE(decoder.open(testFile));

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

      auto decoder = AlacDecoderSession{SampleEncoding::Signed32Le};
      REQUIRE(decoder.open(testFile));

      auto const info = decoder.streamInfo();
      CHECK(info.sourceFormat.precisionBits == 24);
      CHECK(encodingContainerBits(info.outputFormat.encoding) == 32);

      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);
      CHECK(!blockRes->bytes.empty());
    }

    SECTION("Pads 16-bit ALAC samples into 32-bit output")
    {
      auto const testFile = requireAudioFixture("alac16.m4a");

      auto sourceDecoder = AlacDecoderSession{SampleEncoding::Signed16Le};
      REQUIRE(sourceDecoder.open(testFile));

      auto const sourceInfo = sourceDecoder.streamInfo();
      CHECK(sourceInfo.sourceFormat.sampleRate == 44100);
      CHECK(sourceInfo.sourceFormat.channels == 2);
      CHECK(sourceInfo.sourceFormat.precisionBits == 16);

      auto const sourceBlockRes = sourceDecoder.readNextBlock();
      REQUIRE(sourceBlockRes);
      REQUIRE(!sourceBlockRes->bytes.empty());

      auto targetDecoder = AlacDecoderSession{SampleEncoding::Signed32Le};
      REQUIRE(targetDecoder.open(testFile));

      auto const targetInfo = targetDecoder.streamInfo();
      CHECK(encodingContainerBits(targetInfo.outputFormat.encoding) == 32);

      auto const targetBlockRes = targetDecoder.readNextBlock();
      REQUIRE(targetBlockRes);
      REQUIRE(!targetBlockRes->bytes.empty());

      auto const sourceSamples = sourceBlockRes->bytes.size() / sizeof(std::int16_t);
      auto const targetSamples = targetBlockRes->bytes.size() / sizeof(std::int32_t);
      auto const samplesToCheck = std::min({sourceSamples, targetSamples, std::size_t{128}});

      REQUIRE(samplesToCheck > 0);

      auto const* source = reinterpret_cast<std::int16_t const*>(sourceBlockRes->bytes.data());
      auto const* target = reinterpret_cast<std::int32_t const*>(targetBlockRes->bytes.data());

      for (std::size_t index = 0; index < samplesToCheck; ++index)
      {
        CHECK(target[index] == static_cast<std::int32_t>(source[index]) << 16U);
      }
    }
  }

  TEST_CASE("AlacDecoderSession - lifecycle", "[audio][unit][alac]")
  {
    SECTION("Unopened session rejects seek and reports end of stream")
    {
      auto decoder = AlacDecoderSession{SampleEncoding::Signed16Le};
      checkClosedSession(decoder);
    }

    SECTION("Closed session reports end of stream")
    {
      auto const testFile = requireAudioFixture("hires.m4a");

      auto decoder = AlacDecoderSession{SampleEncoding::Signed24PackedLe};
      REQUIRE(decoder.open(testFile));

      decoder.close();
      decoder.close();
      checkClosedSession(decoder);
    }

    SECTION("Reading through the final packet reaches stable end of stream")
    {
      auto const testFile = requireAudioFixture("hires.m4a");

      auto decoder = AlacDecoderSession{SampleEncoding::Signed24PackedLe};
      REQUIRE(decoder.open(testFile));

      CHECK(readUntilStableEndOfStream(decoder, 512) > 0);
    }
  }

  TEST_CASE("AlacDecoderSession - rejects invalid input", "[audio][unit][alac][error]")
  {
    SECTION("Non-existent file")
    {
      auto decoder = AlacDecoderSession{SampleEncoding::Signed16Le};
      CHECK(!decoder.open("/path/to/nowhere/nonexistent.m4a"));
    }

    SECTION("Non-MP4 content")
    {
      auto const garbage = std::vector<std::uint8_t>{'N', 'O', 'T', ' ', 'M', 'P', '4'};
      auto const temp = ao::test::TempFile{garbage, ".m4a"};
      auto decoder = AlacDecoderSession{SampleEncoding::Signed16Le};

      CHECK(!decoder.open(temp.path));
    }

    SECTION("Unsupported 24-bit to 16-bit conversion")
    {
      auto const testFile = requireAudioFixture("hires.m4a");

      auto decoder = AlacDecoderSession{SampleEncoding::Signed16Le};
      CHECK(!decoder.open(testFile));
      checkClosedSession(decoder);
    }

    SECTION("Lossless fixed output requests succeed during open")
    {
      auto const testFile = requireAudioFixture("alac16.m4a");

      CHECK(AlacDecoderSession{SampleEncoding::Float32Le}.open(testFile));
      CHECK(AlacDecoderSession{SampleEncoding::Signed24In32Le}.open(testFile));
    }

    SECTION("Failed reopen clears the previous stream state")
    {
      auto const testFile = requireAudioFixture("alac16.m4a");
      auto decoder = AlacDecoderSession{SampleEncoding::Signed16Le};

      REQUIRE(decoder.open(testFile));
      CHECK(!decoder.open("/path/to/nowhere/nonexistent.m4a"));
      checkClosedSession(decoder);
    }
  }
} // namespace ao::audio::test
