// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/media/wav/Riff.h>

#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/media/wav/TestWav.h"
#include <ao/Error.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace ao::media::wav::test
{
  namespace
  {
    // Authors the sample rate and byte rate at fmt offsets 4..7 and 8..11.
    // Malformed declarations do not depend on wrapping builder arithmetic.
    void setDeclaredFmtRates(std::vector<std::uint8_t>& fmtPayload,
                             std::uint32_t const sampleRate,
                             std::uint32_t const byteRate)
    {
      fmtPayload[4] = static_cast<std::uint8_t>(sampleRate & 0xFFU);
      fmtPayload[5] = static_cast<std::uint8_t>((sampleRate >> 8U) & 0xFFU);
      fmtPayload[6] = static_cast<std::uint8_t>((sampleRate >> 16U) & 0xFFU);
      fmtPayload[7] = static_cast<std::uint8_t>((sampleRate >> 24U) & 0xFFU);
      fmtPayload[8] = static_cast<std::uint8_t>(byteRate & 0xFFU);
      fmtPayload[9] = static_cast<std::uint8_t>((byteRate >> 8U) & 0xFFU);
      fmtPayload[10] = static_cast<std::uint8_t>((byteRate >> 16U) & 0xFFU);
      fmtPayload[11] = static_cast<std::uint8_t>((byteRate >> 24U) & 0xFFU);
    }
  } // namespace

  TEST_CASE("WAV RIFF parser - reads real PCM fixture layout", "[media][unit][wav]")
  {
    auto const bytes = audio::test::readFileBytes(audio::test::requireAudioFixture("basic_metadata.wav"));
    auto parsedRes = parseWave(utility::bytes::view(bytes.data(), bytes.size()));

    REQUIRE(parsedRes);
    auto const& parsed = *parsedRes;
    CHECK(parsed.format.formatTag == kFormatPcm);
    CHECK(parsed.format.sampleRate == 44100);
    CHECK(parsed.format.channels == 2);
    CHECK(parsed.format.bitsPerSample == 16);
    CHECK(parsed.format.validBitsPerSample == 16);
    CHECK_FALSE(parsed.format.isFloat);
    CHECK_FALSE(parsed.format.optChannelMask);
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
    auto parsedRes = parseWave(utility::bytes::view(bytes.data(), bytes.size()));

    REQUIRE(parsedRes);
    auto const& parsed = *parsedRes;
    CHECK(parsed.format.formatTag == kFormatPcm);
    CHECK(parsed.format.sampleRate == 96000);
    CHECK(parsed.format.channels == 2);
    CHECK(parsed.format.bitsPerSample == 24);
    CHECK(parsed.format.validBitsPerSample == 24);
    CHECK_FALSE(parsed.format.isFloat);
    // The fixture stores an extensible fmt chunk declaring a raw stereo mask.
    REQUIRE(parsed.format.optChannelMask);
    CHECK(*parsed.format.optChannelMask == 0x3U);
    CHECK((parsed.data.size() % parsed.format.blockAlign) == 0);
  }

  TEST_CASE("WAV RIFF parser - reads real IEEE float fixture layout", "[media][unit][wav]")
  {
    auto const bytes = audio::test::readFileBytes(audio::test::requireAudioFixture("float32.wav"));
    auto parsedRes = parseWave(utility::bytes::view(bytes.data(), bytes.size()));

    REQUIRE(parsedRes);
    auto const& parsed = *parsedRes;
    CHECK(parsed.format.formatTag == kFormatIeeeFloat);
    CHECK(parsed.format.sampleRate == 48000);
    CHECK(parsed.format.channels == 2);
    CHECK(parsed.format.bitsPerSample == 32);
    CHECK(parsed.format.validBitsPerSample == 32);
    CHECK(parsed.format.isFloat);
    CHECK_FALSE(parsed.format.optChannelMask);
    CHECK_FALSE(parsed.data.empty());
  }

  TEST_CASE("WAV RIFF parser - required audio extent ignores malformed trailing chunks", "[media][unit][wav]")
  {
    auto data = ao::test::wav::makeWav({});
    ao::test::wav::appendTruncatedChunk(data, "JUNK", 100);

    auto requiredRes = parseWave(utility::bytes::view(data.data(), data.size()), WaveParseExtent::RequiredAudio);
    auto completeRes = parseWave(utility::bytes::view(data.data(), data.size()), WaveParseExtent::Complete);

    REQUIRE(requiredRes);
    CHECK(requiredRes->data.size() == 2);
    REQUIRE_FALSE(completeRes);
    CHECK(completeRes.error().code == Error::Code::CorruptData);
  }

  TEST_CASE("WAV RIFF parser - skips empty data chunks before required audio", "[media][unit][wav]")
  {
    auto const emptyData = ao::test::wav::Chunk{.id = {'d', 'a', 't', 'a'}, .payload = {}};
    auto const data = ao::test::wav::makeWav({.extraChunks = {emptyData}});

    auto requiredRes = parseWave(utility::bytes::view(data.data(), data.size()), WaveParseExtent::RequiredAudio);
    auto completeRes = parseWave(utility::bytes::view(data.data(), data.size()), WaveParseExtent::Complete);

    REQUIRE(requiredRes);
    CHECK(requiredRes->data.size() == 2);
    REQUIRE(completeRes);
    CHECK(completeRes->data.size() == 2);
  }

  TEST_CASE("WAV RIFF parser - rejects malformed or unsupported RIFF data", "[media][unit][wav]")
  {
    SECTION("empty audio data")
    {
      auto data = ao::test::wav::makeWav({.audioData = {}});
      auto res = parseWave(utility::bytes::view(data.data(), data.size()));

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::CorruptData);
    }

    SECTION("truncated RIFF body")
    {
      auto data = ao::test::wav::makeWav({});
      data.pop_back();
      auto res = parseWave(utility::bytes::view(data.data(), data.size()));

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::CorruptData);
    }

    SECTION("unsupported extensible subformat")
    {
      auto data = ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::UnsupportedExtensible});
      auto res = parseWave(utility::bytes::view(data.data(), data.size()));

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotSupported);
    }

    SECTION("extensible valid bits narrower than the container")
    {
      auto data = ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                          .bitsPerSample = 32,
                                          .validBitsPerSample = 24,
                                          .audioData = {0, 0, 0, 0}});
      auto res = parseWave(utility::bytes::view(data.data(), data.size()));

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotSupported);
    }

    SECTION("channel count cannot be represented by Aobus audio formats")
    {
      auto data = ao::test::wav::makeWav({.channels = 256});
      auto res = parseWave(utility::bytes::view(data.data(), data.size()));

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::NotSupported);
    }

    SECTION("duplicate fmt chunks")
    {
      auto duplicateFmt = ao::test::wav::makeFmtChunk({});
      auto data = ao::test::wav::makeWav({.extraChunks = {{{.id = {'f', 'm', 't', ' '}, .payload = duplicateFmt}}}});
      auto res = parseWave(utility::bytes::view(data.data(), data.size()));

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::CorruptData);
    }

    SECTION("duplicate data chunks")
    {
      auto data = ao::test::wav::makeWav({
        .extraChunks = {{{.id = {'d', 'a', 't', 'a'}, .payload = {0, 0}}}},
      });
      auto res = parseWave(utility::bytes::view(data.data(), data.size()));

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::CorruptData);
    }
  }

  TEST_CASE("WAV fixture builder - rejects computed byte rates beyond DWORD storage", "[media][unit][wav]")
  {
    auto spec = ao::test::wav::Spec{.sampleRate = 0x800003E8U};

    SECTION("legacy mono 16-bit product exceeds the DWORD maximum")
    {
      spec.channels = 1;
    }

    SECTION("extensible stereo 16-bit product exceeds the DWORD maximum")
    {
      spec.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm;
      spec.channels = 2;
    }

    REQUIRE_THROWS_AS(ao::test::wav::makeFmtChunk(spec), std::overflow_error);
    REQUIRE_THROWS_AS(ao::test::wav::makeWav(spec), std::overflow_error);
  }

  TEST_CASE("WAV RIFF parser - validates declared fmt byte rate against full-precision sample rate product",
            "[media][unit][wav]")
  {
    // Every variant authors the declared byte rate as literal fmt payload
    // bytes; the expectation is never a product recomputed with the 32-bit
    // arithmetic under test. Marker audio carries two whole frames.
    auto spec = ao::test::wav::Spec{};
    std::uint32_t declaredSampleRate = 0;
    std::uint32_t declaredByteRate = 0;
    std::uint16_t expectedBlockAlign = 0;
    std::size_t expectedDataOffset = 0;
    bool coherentLayout = false;

    SECTION("coherent mono byte rate is accepted")
    {
      spec = ao::test::wav::Spec{
        .sampleFormat = ao::test::wav::SampleFormat::Pcm,
        .channels = 1,
        .sampleRate = 48000,
        .bitsPerSample = 16,
        .audioData = {0x64, 0x00, 0x65, 0x00}, // 16-bit samples 100, 101
      };
      declaredSampleRate = 48000;
      declaredByteRate = 96000;
      expectedBlockAlign = 2;
      expectedDataOffset = 44;
      coherentLayout = true;
    }

    SECTION("highest representable mono 16-bit byte-rate product is accepted")
    {
      // sampleRate 0x7FFFFFFF * blockAlign 2 = 0xFFFFFFFE, the largest
      // representable product for a two-byte block alignment.
      spec = ao::test::wav::Spec{
        .sampleFormat = ao::test::wav::SampleFormat::Pcm,
        .channels = 1,
        .sampleRate = 0x7FFFFFFFU,
        .bitsPerSample = 16,
        .audioData = {0x64, 0x00, 0x65, 0x00},
      };
      declaredSampleRate = 0x7FFFFFFFU;
      declaredByteRate = 0xFFFFFFFEU;
      expectedBlockAlign = 2;
      expectedDataOffset = 44;
      coherentLayout = true;
    }

    SECTION("coherent extensible stereo byte rate is accepted")
    {
      spec = ao::test::wav::Spec{
        .sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
        .channels = 2,
        .sampleRate = 48000,
        .bitsPerSample = 16,
        .channelMask = 0x3,
        .audioData = {0x64, 0x00, 0x38, 0xFF, 0x65, 0x00, 0x37, 0xFF},
      };
      declaredSampleRate = 48000;
      declaredByteRate = 192000;
      expectedBlockAlign = 4;
      expectedDataOffset = 68;
      coherentLayout = true;
    }

    SECTION("mono 8-bit byte rate at the DWORD maximum is accepted")
    {
      spec = ao::test::wav::Spec{
        .sampleFormat = ao::test::wav::SampleFormat::Pcm,
        .channels = 1,
        .sampleRate = 0xFFFFFFFFU,
        .bitsPerSample = 8,
        .audioData = {0x64, 0x65},
      };
      declaredSampleRate = 0xFFFFFFFFU;
      declaredByteRate = 0xFFFFFFFFU;
      expectedBlockAlign = 1;
      expectedDataOffset = 44;
      coherentLayout = true;
    }

    SECTION("inconsistent non-overflowing byte rate is rejected")
    {
      spec = ao::test::wav::Spec{
        .sampleFormat = ao::test::wav::SampleFormat::Pcm,
        .channels = 1,
        .sampleRate = 48000,
        .bitsPerSample = 16,
        .audioData = {0x64, 0x00, 0x65, 0x00},
      };
      declaredSampleRate = 48000;
      declaredByteRate = 96001;
    }

    SECTION("mono byte rate matching the wrapped high-rate product is rejected")
    {
      // sampleRate 0x800003E8 * blockAlign 2 = 2^32 + 2000, so a 32-bit
      // product wraps to 2000. No declared byte rate can be coherent here.
      spec = ao::test::wav::Spec{
        .sampleFormat = ao::test::wav::SampleFormat::Pcm,
        .channels = 1,
        .sampleRate = 1000, // Build a representable prefix; author the corrupt rate below.
        .bitsPerSample = 16,
        .audioData = {0x64, 0x00, 0x65, 0x00},
      };
      declaredSampleRate = 0x800003E8U;
      declaredByteRate = 2000;
    }

    SECTION("extensible stereo byte rate matching the wrapped product is rejected")
    {
      // sampleRate 0x800003E8 * blockAlign 4 = 2^33 + 4000, which wraps to
      // 4000 in 32-bit arithmetic.
      spec = ao::test::wav::Spec{
        .sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
        .channels = 2,
        .sampleRate = 1000, // Build a representable prefix; author the corrupt rate below.
        .bitsPerSample = 16,
        .channelMask = 0x3,
        .audioData = {0x64, 0x00, 0x38, 0xFF, 0x65, 0x00, 0x37, 0xFF},
      };
      declaredSampleRate = 0x800003E8U;
      declaredByteRate = 4000;
    }

    auto fmtPayload = ao::test::wav::makeFmtChunk(spec);
    setDeclaredFmtRates(fmtPayload, declaredSampleRate, declaredByteRate);
    auto const data = ao::test::wav::makeWav(spec, fmtPayload);

    for (auto const extent : {WaveParseExtent::Complete, WaveParseExtent::RequiredAudio})
    {
      if (auto const parsedRes = parseWave(utility::bytes::view(data.data(), data.size()), extent); coherentLayout)
      {
        REQUIRE(parsedRes);
        CHECK(parsedRes->format.channels == spec.channels);
        CHECK(parsedRes->format.sampleRate == declaredSampleRate);
        CHECK(parsedRes->format.bitsPerSample == spec.bitsPerSample);
        CHECK(parsedRes->format.byteRate == declaredByteRate);
        CHECK(parsedRes->format.blockAlign == expectedBlockAlign);
        CHECK(parsedRes->dataOffset == expectedDataOffset);
        CHECK(std::ranges::equal(parsedRes->data, utility::bytes::view(spec.audioData.data(), spec.audioData.size())));
        REQUIRE(parsedRes->format.blockAlign > 0);
        CHECK((parsedRes->data.size() % parsedRes->format.blockAlign) == 0);
        CHECK((parsedRes->data.size() / parsedRes->format.blockAlign) == 2);
      }
      else
      {
        REQUIRE_FALSE(parsedRes);
        CHECK(parsedRes.error().code == Error::Code::CorruptData);
      }
    }
  }
} // namespace ao::media::wav::test
