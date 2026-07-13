// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "DecoderTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/media/wav/TestWav.h"
#include <ao/AudioCodec.h>
#include <ao/audio/Format.h>
#include <ao/audio/WavDecoderSession.h>
#include <ao/media/wav/Riff.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    std::span<std::byte const> asBytes(std::vector<std::uint8_t> const& bytes) noexcept
    {
      return utility::bytes::view(std::span{bytes});
    }

    struct ParsedWaveFixture final
    {
      std::vector<std::uint8_t> bytes;
      media::wav::ParsedWave wave = {};
    };

    ParsedWaveFixture requireParsedWave(std::filesystem::path const& path)
    {
      auto fixture = ParsedWaveFixture{.bytes = readFileBytes(path)};
      auto result = media::wav::parseWave(asBytes(fixture.bytes));
      REQUIRE(result);
      fixture.wave = *result;
      return fixture;
    }

    std::int16_t readLe16(std::span<std::byte const> bytes, std::size_t offset) noexcept
    {
      auto const bits = static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset])) |
                        static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset + 1U]) << 8U);
      return static_cast<std::int16_t>(bits);
    }

    std::vector<std::uint8_t> floatSamples(std::span<float const> samples)
    {
      auto data = std::vector<std::uint8_t>{};

      for (float const sample : samples)
      {
        auto const bits = std::bit_cast<std::uint32_t>(sample);
        data.push_back(static_cast<std::uint8_t>(bits & 0xFFU));
        data.push_back(static_cast<std::uint8_t>((bits >> 8U) & 0xFFU));
        data.push_back(static_cast<std::uint8_t>((bits >> 16U) & 0xFFU));
        data.push_back(static_cast<std::uint8_t>((bits >> 24U) & 0xFFU));
      }

      return data;
    }
  } // namespace

  TEST_CASE("WavDecoderSession - decodes real 16-bit PCM without rewriting bytes", "[audio][unit][wav]")
  {
    auto const fixture = requireAudioFixture("basic_metadata.wav");
    auto decoder = WavDecoderSession{Format{.isInterleaved = true}};

    REQUIRE(decoder.open(fixture));
    auto const info = decoder.streamInfo();
    CHECK(info.codec == AudioCodec::Wav);
    CHECK_FALSE(info.isLossy);
    CHECK(info.sourceFormat.sampleRate == 44100);
    CHECK(info.sourceFormat.channels == 2);
    CHECK(info.sourceFormat.bitDepth == 16);
    CHECK(info.outputFormat == info.sourceFormat);

    auto const parsed = requireParsedWave(fixture);
    auto blockResult = decoder.readNextBlock();
    REQUIRE(blockResult);
    auto const& block = *blockResult;
    REQUIRE(block.bytes.size() <= parsed.wave.data.size());
    CHECK(block.firstFrameIndex == 0);
    CHECK(block.frames > 0);
    CHECK(std::ranges::equal(block.bytes, parsed.wave.data.first(block.bytes.size())));
  }

  TEST_CASE("WavDecoderSession - decodes real extensible 24-bit PCM", "[audio][unit][wav]")
  {
    auto const fixture = requireAudioFixture("hires.wav");
    auto decoder = WavDecoderSession{Format{.bitDepth = 24, .isInterleaved = true}};

    REQUIRE(decoder.open(fixture));
    auto const info = decoder.streamInfo();
    CHECK(info.codec == AudioCodec::Wav);
    CHECK(info.sourceFormat.sampleRate == 96000);
    CHECK(info.sourceFormat.channels == 2);
    CHECK(info.sourceFormat.bitDepth == 24);
    CHECK(info.sourceFormat.validBits == 24);
    CHECK(info.outputFormat.bitDepth == 24);
    CHECK(info.outputFormat.validBits == 24);

    auto const parsed = requireParsedWave(fixture);
    auto blockResult = decoder.readNextBlock();
    REQUIRE(blockResult);
    auto const& block = *blockResult;
    CHECK(block.bitDepth == 24);
    REQUIRE(block.bytes.size() <= parsed.wave.data.size());
    CHECK(std::ranges::equal(block.bytes, parsed.wave.data.first(block.bytes.size())));
  }

  TEST_CASE("WavDecoderSession - preserves real 32-bit float PCM", "[audio][unit][wav]")
  {
    auto const fixture = requireAudioFixture("float32.wav");
    auto decoder = WavDecoderSession{Format{.isInterleaved = true}};

    REQUIRE(decoder.open(fixture));
    auto const info = decoder.streamInfo();
    CHECK(info.codec == AudioCodec::Wav);
    CHECK(info.sourceFormat.sampleRate == 48000);
    CHECK(info.sourceFormat.isFloat);
    CHECK(info.outputFormat.isFloat);
    CHECK(info.outputFormat.bitDepth == 32);
    CHECK(info.outputFormat.validBits == 32);

    auto const parsed = requireParsedWave(fixture);
    auto blockResult = decoder.readNextBlock();
    REQUIRE(blockResult);
    auto const& block = *blockResult;
    REQUIRE(block.bytes.size() <= parsed.wave.data.size());
    CHECK(std::ranges::equal(block.bytes, parsed.wave.data.first(block.bytes.size())));
  }

  TEST_CASE("WavDecoderSession - converts 32-bit float PCM to requested integer output", "[audio][unit][wav]")
  {
    auto const samples = std::vector{-1.0F, 0.0F, 1.0F, 0.5F};
    auto data = ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::IeeeFloat,
                                        .bitsPerSample = 32,
                                        .validBitsPerSample = 32,
                                        .audioData = floatSamples(std::span{samples})});
    auto const temp = ao::test::TempFile{data, ".wav"};
    auto decoder = WavDecoderSession{Format{.bitDepth = 16, .validBits = 16, .isInterleaved = true}};

    REQUIRE(decoder.open(temp.path));
    auto const info = decoder.streamInfo();
    CHECK(info.sourceFormat.isFloat);
    CHECK_FALSE(info.outputFormat.isFloat);
    CHECK(info.outputFormat.bitDepth == 16);
    CHECK(info.outputFormat.validBits == 16);

    auto blockResult = decoder.readNextBlock();
    REQUIRE(blockResult);
    auto const& block = *blockResult;
    REQUIRE(block.bytes.size() == samples.size() * sizeof(std::int16_t));
    CHECK(readLe16(block.bytes, 0) == -32768);
    CHECK(readLe16(block.bytes, 2) == 0);
    CHECK(readLe16(block.bytes, 4) == 32767);
    CHECK(readLe16(block.bytes, 6) == 16384);
  }

  TEST_CASE("WavDecoderSession - converts real unsigned 8-bit PCM to signed 16-bit output", "[audio][unit][wav]")
  {
    auto const fixture = requireAudioFixture("u8.wav");
    auto decoder = WavDecoderSession{Format{.isInterleaved = true}};

    REQUIRE(decoder.open(fixture));
    auto const info = decoder.streamInfo();
    CHECK(info.sourceFormat.bitDepth == 8);
    CHECK(info.sourceFormat.validBits == 8);
    CHECK(info.outputFormat.bitDepth == 16);
    CHECK(info.outputFormat.validBits == 8);

    auto const parsed = requireParsedWave(fixture);
    auto blockResult = decoder.readNextBlock();
    REQUIRE(blockResult);
    auto const& block = *blockResult;
    REQUIRE(block.frames > 8);
    REQUIRE(block.bytes.size() >= 16);

    for (std::size_t index = 0; index < 8; ++index)
    {
      auto const unsignedSample = static_cast<std::int16_t>(std::to_integer<std::uint8_t>(parsed.wave.data[index]));
      auto const expected = static_cast<std::int16_t>((unsignedSample - 128) << 8U);
      CHECK(readLe16(block.bytes, index * 2U) == expected);
    }
  }

  TEST_CASE("WavDecoderSession - ignores malformed chunks after required audio data", "[audio][regression][wav]")
  {
    auto data = ao::test::wav::makeWav({});
    ao::test::wav::appendTruncatedChunk(data, "JUNK", 100);
    auto const temp = ao::test::TempFile{data, ".wav"};
    auto decoder = WavDecoderSession{Format{.isInterleaved = true}};

    auto const openResult = decoder.open(temp.path);

    REQUIRE(openResult);
    CHECK(decoder.streamInfo().codec == AudioCodec::Wav);
    auto blockResult = decoder.readNextBlock();
    REQUIRE(blockResult);
    CHECK(blockResult->frames > 0);
  }

  TEST_CASE("WavDecoderSession - seek and end-of-stream are stable", "[audio][unit][wav]")
  {
    auto decoder = WavDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

    REQUIRE(decoder.open(requireAudioFixture("basic_metadata.wav")));
    REQUIRE(decoder.seek(std::chrono::milliseconds{500}));
    auto blockResult = decoder.readNextBlock();
    REQUIRE(blockResult);
    CHECK(blockResult->firstFrameIndex == 22050);
    CHECK(readUntilStableEndOfStream(decoder, 512) > 0);
  }

  TEST_CASE("WavDecoderSession - reports unsupported fixed output requests", "[audio][unit][wav][error]")
  {
    auto const integerFixture = requireAudioFixture("basic_metadata.wav");
    auto const floatFixture = requireAudioFixture("float32.wav");

    CHECK((!WavDecoderSession{Format{.sampleRate = 1, .bitDepth = 16, .isInterleaved = true}}.open(integerFixture)));
    CHECK((!WavDecoderSession{Format{.channels = 1, .bitDepth = 16, .isInterleaved = true}}.open(integerFixture)));
    CHECK((!WavDecoderSession{Format{.bitDepth = 16, .isInterleaved = false}}.open(integerFixture)));
    CHECK((!WavDecoderSession{Format{.bitDepth = 32, .isFloat = true, .isInterleaved = true}}.open(integerFixture)));
    CHECK((!WavDecoderSession{Format{.bitDepth = 24, .validBits = 16, .isInterleaved = true}}.open(floatFixture)));

    auto decoder = WavDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
    CHECK(!decoder.open("/path/to/nowhere/nonexistent.wav"));
    checkClosedSession(decoder);
  }
} // namespace ao::audio::test
