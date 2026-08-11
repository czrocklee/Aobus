// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/WavDecoderSession.h"

#include "DecoderTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/media/wav/TestWav.h"
#include <ao/AudioCodec.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/media/wav/Riff.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
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
    auto decoderPtr = ao::test::requireValue(WavDecoderSession::open(fixture, std::nullopt));
    auto& decoder = *decoderPtr;
    auto const info = decoder.streamInfo();
    CHECK(info.codec == AudioCodec::Wav);
    CHECK_FALSE(info.isLossy);
    CHECK(info.sourceFormat.sampleRate == 44100);
    CHECK(info.sourceFormat.channels == 2);
    CHECK(info.sourceFormat.precisionBits == 16);
    CHECK(signalFormat(info.outputFormat) == info.sourceFormat);
    CHECK(info.outputFormat.encoding == SampleEncoding::Signed16Le);

    auto const parsed = requireParsedWave(fixture);
    auto blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    auto const& block = *blockRes;
    REQUIRE(block.bytes.size() <= parsed.wave.data.size());
    CHECK(block.firstFrameIndex == 0);
    CHECK(block.frames > 0);
    CHECK(std::ranges::equal(block.bytes, parsed.wave.data.first(block.bytes.size())));
  }

  TEST_CASE("WavDecoderSession - decodes real extensible 24-bit PCM", "[audio][unit][wav]")
  {
    auto const fixture = requireAudioFixture("hires.wav");
    auto decoderPtr = ao::test::requireValue(WavDecoderSession::open(fixture, SampleEncoding::Signed24PackedLe));
    auto& decoder = *decoderPtr;
    auto const info = decoder.streamInfo();
    CHECK(info.codec == AudioCodec::Wav);
    CHECK(info.sourceFormat.sampleRate == 96000);
    CHECK(info.sourceFormat.channels == 2);
    CHECK(info.sourceFormat.precisionBits == 24);
    CHECK(encodingContainerBits(info.outputFormat.encoding) == 24);

    auto const parsed = requireParsedWave(fixture);
    auto blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    auto const& block = *blockRes;
    CHECK(info.outputFormat.encoding == SampleEncoding::Signed24PackedLe);
    REQUIRE(block.bytes.size() <= parsed.wave.data.size());
    CHECK(std::ranges::equal(block.bytes, parsed.wave.data.first(block.bytes.size())));
  }

  TEST_CASE("WavDecoderSession - preserves 32-bit integer PCM", "[audio][unit][wav]")
  {
    auto const audioData = std::vector<std::uint8_t>{
      0x00,
      0x00,
      0x00,
      0x00, // zero
      0xFF,
      0xFF,
      0xFF,
      0x7F, // positive extreme
      0x00,
      0x00,
      0x00,
      0x80, // negative extreme
      0x78,
      0x56,
      0x34,
      0x12, // non-trivial byte order
    };
    auto data = ao::test::wav::makeWav({.bitsPerSample = 32, .audioData = audioData});
    auto const temp = ao::test::TempFile{data, ".wav"};
    auto decoderPtr = ao::test::requireValue(WavDecoderSession::open(temp.path, SampleEncoding::Signed32Le));
    auto& decoder = *decoderPtr;
    auto blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK(blockRes->frames == 4);
    CHECK(blockRes->endOfStream);
    CHECK(std::ranges::equal(blockRes->bytes, asBytes(audioData)));
  }

  TEST_CASE("WavDecoderSession - rejects precision-losing integer output", "[audio][unit][wav]")
  {
    auto const audioData = std::vector<std::uint8_t>{
      0xFF,
      0xFF,
      0x7F,
      0x00,
      0x00,
      0x80,
      0x00,
      0x00,
      0x00,
    };
    auto data = ao::test::wav::makeWav({.bitsPerSample = 24, .audioData = audioData});
    auto const temp = ao::test::TempFile{data, ".wav"};
    auto const result = WavDecoderSession::open(temp.path, SampleEncoding::Signed16Le);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotSupported);
  }

  TEST_CASE("WavDecoderSession - preserves a partial final passthrough block", "[audio][unit][wav]")
  {
    constexpr std::size_t kFrameCount = 4097;
    auto audioData = std::vector<std::uint8_t>(kFrameCount * sizeof(std::int16_t));

    for (std::size_t index = 0; index < audioData.size(); ++index)
    {
      audioData[index] = static_cast<std::uint8_t>(index % 251U);
    }

    auto data = ao::test::wav::makeWav({.bitsPerSample = 16, .audioData = audioData});
    auto const temp = ao::test::TempFile{data, ".wav"};
    auto decoderPtr = ao::test::requireValue(WavDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    auto firstRes = decoder.readNextBlock();
    REQUIRE(firstRes);
    CHECK(firstRes->frames == 4096);
    CHECK_FALSE(firstRes->endOfStream);
    CHECK(std::ranges::equal(firstRes->bytes, asBytes(audioData).first(4096 * sizeof(std::int16_t))));

    auto finalRes = decoder.readNextBlock();
    REQUIRE(finalRes);
    CHECK(finalRes->frames == 1);
    CHECK(finalRes->endOfStream);
    CHECK(std::ranges::equal(finalRes->bytes, asBytes(audioData).last(sizeof(std::int16_t))));
  }

  TEST_CASE("WavDecoderSession - preserves real 32-bit float PCM", "[audio][unit][wav]")
  {
    auto const fixture = requireAudioFixture("float32.wav");
    auto decoderPtr = ao::test::requireValue(WavDecoderSession::open(fixture, std::nullopt));
    auto& decoder = *decoderPtr;
    auto const info = decoder.streamInfo();
    CHECK(info.codec == AudioCodec::Wav);
    CHECK(info.sourceFormat.sampleRate == 48000);
    CHECK(info.sourceFormat.sampleKind == SampleKind::FloatingPoint);
    CHECK(isFloatEncoding(info.outputFormat.encoding));
    CHECK(encodingContainerBits(info.outputFormat.encoding) == 32);

    auto const parsed = requireParsedWave(fixture);
    auto blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    auto const& block = *blockRes;
    REQUIRE(block.bytes.size() <= parsed.wave.data.size());
    CHECK(std::ranges::equal(block.bytes, parsed.wave.data.first(block.bytes.size())));
  }

  TEST_CASE("WavDecoderSession - rejects float-to-integer output", "[audio][unit][wav]")
  {
    auto const samples = std::vector{-1.0F, 0.0F, 1.0F, 0.5F};
    auto data = ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::IeeeFloat,
                                        .bitsPerSample = 32,
                                        .validBitsPerSample = 32,
                                        .audioData = floatSamples(std::span{samples})});
    auto const temp = ao::test::TempFile{data, ".wav"};
    auto const result = WavDecoderSession::open(temp.path, SampleEncoding::Signed16Le);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotSupported);
  }

  TEST_CASE("WavDecoderSession - converts real unsigned 8-bit PCM to signed 16-bit output", "[audio][unit][wav]")
  {
    auto const fixture = requireAudioFixture("u8.wav");
    auto decoderPtr = ao::test::requireValue(WavDecoderSession::open(fixture, std::nullopt));
    auto& decoder = *decoderPtr;
    auto const info = decoder.streamInfo();
    CHECK(info.sourceFormat.precisionBits == 8);
    CHECK(encodingContainerBits(info.outputFormat.encoding) == 16);

    auto const parsed = requireParsedWave(fixture);
    auto blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    auto const& block = *blockRes;
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
    auto decoderPtr = ao::test::requireValue(WavDecoderSession::open(temp.path, std::nullopt));
    auto& decoder = *decoderPtr;
    CHECK(decoder.streamInfo().codec == AudioCodec::Wav);
    auto blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK(blockRes->frames > 0);
  }

  TEST_CASE("WavDecoderSession - seek and end-of-stream are stable", "[audio][unit][wav]")
  {
    auto decoderPtr = ao::test::requireValue(
      WavDecoderSession::open(requireAudioFixture("basic_metadata.wav"), SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;
    REQUIRE(decoder.seek(std::chrono::milliseconds{500}));
    auto blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK(blockRes->firstFrameIndex == 22050);
    CHECK(readUntilStableEndOfStream(decoder, 512) > 0);
  }

  TEST_CASE("WavDecoderSession - reports invalid input", "[audio][unit][wav][error]")
  {
    auto const integerFixture = requireAudioFixture("basic_metadata.wav");
    CHECK(WavDecoderSession::open(integerFixture, SampleEncoding::Float32Le));

    CHECK(!WavDecoderSession::open("/path/to/nowhere/nonexistent.wav", SampleEncoding::Signed16Le));
  }
} // namespace ao::audio::test
