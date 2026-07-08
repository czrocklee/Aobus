// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/media/wav/TestWav.h"
#include <ao/Error.h>
#include <ao/media/wav/Riff.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ao::media::wav::test
{
  namespace
  {
    std::span<std::byte const> asBytes(std::vector<std::uint8_t> const& bytes) noexcept
    {
      return utility::bytes::view(std::span{bytes});
    }
  } // namespace

  TEST_CASE("WAV RIFF parser - reads real PCM fixture layout", "[media][unit][wav]")
  {
    auto const bytes = audio::test::readFileBytes(audio::test::requireAudioFixture("basic_metadata.wav"));
    auto parsedResult = parseWave(asBytes(bytes));

    REQUIRE(parsedResult);
    auto const& parsed = *parsedResult;
    CHECK(parsed.format.formatTag == kFormatPcm);
    CHECK(parsed.format.sampleRate == 44100);
    CHECK(parsed.format.channels == 2);
    CHECK(parsed.format.bitsPerSample == 16);
    CHECK(parsed.format.validBitsPerSample == 16);
    CHECK_FALSE(parsed.format.isFloat);
    CHECK(parsed.dataOffset > 0);
    CHECK_FALSE(parsed.data.empty());

    bool hasInfoList = false;

    for (auto const& chunk : parsed.chunks)
    {
      hasInfoList = hasInfoList || hasChunkId(chunk, "LIST");
    }

    CHECK(hasInfoList);
  }

  TEST_CASE("WAV RIFF parser - resolves real extensible PCM fixture layout", "[media][unit][wav]")
  {
    auto const bytes = audio::test::readFileBytes(audio::test::requireAudioFixture("hires.wav"));
    auto parsedResult = parseWave(asBytes(bytes));

    REQUIRE(parsedResult);
    auto const& parsed = *parsedResult;
    CHECK(parsed.format.formatTag == kFormatPcm);
    CHECK(parsed.format.sampleRate == 96000);
    CHECK(parsed.format.channels == 2);
    CHECK(parsed.format.bitsPerSample == 24);
    CHECK(parsed.format.validBitsPerSample == 24);
    CHECK_FALSE(parsed.format.isFloat);
    CHECK((parsed.data.size() % parsed.format.blockAlign) == 0);
  }

  TEST_CASE("WAV RIFF parser - reads real IEEE float fixture layout", "[media][unit][wav]")
  {
    auto const bytes = audio::test::readFileBytes(audio::test::requireAudioFixture("float32.wav"));
    auto parsedResult = parseWave(asBytes(bytes));

    REQUIRE(parsedResult);
    auto const& parsed = *parsedResult;
    CHECK(parsed.format.formatTag == kFormatIeeeFloat);
    CHECK(parsed.format.sampleRate == 48000);
    CHECK(parsed.format.channels == 2);
    CHECK(parsed.format.bitsPerSample == 32);
    CHECK(parsed.format.validBitsPerSample == 32);
    CHECK(parsed.format.isFloat);
    CHECK_FALSE(parsed.data.empty());
  }

  TEST_CASE("WAV RIFF parser - rejects malformed or unsupported RIFF data", "[media][unit][wav]")
  {
    SECTION("empty audio data")
    {
      auto data = ao::test::wav::makeWav({.audioData = {}});
      auto result = parseWave(asBytes(data));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("truncated RIFF body")
    {
      auto data = ao::test::wav::makeWav({});
      data.pop_back();
      auto result = parseWave(asBytes(data));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("unsupported extensible subformat")
    {
      auto data = ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::UnsupportedExtensible});
      auto result = parseWave(asBytes(data));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    SECTION("extensible valid bits narrower than the container")
    {
      auto data = ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                          .bitsPerSample = 32,
                                          .validBitsPerSample = 24,
                                          .audioData = {0, 0, 0, 0}});
      auto result = parseWave(asBytes(data));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    SECTION("channel count cannot be represented by Aobus audio formats")
    {
      auto data = ao::test::wav::makeWav({.channels = 256});
      auto result = parseWave(asBytes(data));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    SECTION("duplicate fmt chunks")
    {
      auto duplicateFmt = ao::test::wav::makeFmtChunk({});
      auto data = ao::test::wav::makeWav({.extraChunks = {{{.id = {'f', 'm', 't', ' '}, .payload = duplicateFmt}}}});
      auto result = parseWave(asBytes(data));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("duplicate data chunks")
    {
      auto data = ao::test::wav::makeWav({
        .extraChunks = {{{.id = {'d', 'a', 't', 'a'}, .payload = {0, 0}}}},
      });
      auto result = parseWave(asBytes(data));

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }
  }
} // namespace ao::media::wav::test
