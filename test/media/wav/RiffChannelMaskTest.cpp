// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/media/wav/TestWav.h"
#include <ao/Error.h>
#include <ao/media/wav/Riff.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ao::media::wav::test
{
  namespace
  {
    // File offset of the fmt chunk's size field in a file built by
    // ao::test::wav::makeWav with no extra chunks: the RIFF header occupies
    // file bytes 0..11 and the fmt chunk header starts at file offset 12.
    constexpr std::size_t kFmtChunkSizeFieldOffset = 16;
    constexpr std::size_t kExtensibleFmtPayloadSize = 40;

    // Authored marker audio fixtures: exact literal bytes, independent of any
    // production conversion. Every fixture carries two frames so parsed frame
    // geometry stays observable.
    constexpr auto kStereoMarkerAudio = std::to_array<std::uint8_t>({
      0x64,
      0x00,
      0x38,
      0xFF, // frame 1: 16-bit samples 100, -200
      0x65,
      0x00,
      0x37,
      0xFF, // frame 2: 16-bit samples 101, -201
    });
    constexpr auto kQuadMarkerAudio = std::to_array<std::uint8_t>({
      0x64,
      0x00,
      0x38,
      0xFF,
      0x2C,
      0x01,
      0x70,
      0xFE, // frame 1: 100, -200, 300, -400
      0x65,
      0x00,
      0x37,
      0xFF,
      0x2D,
      0x01,
      0x6F,
      0xFE, // frame 2: 101, -201, 301, -401
    });
    constexpr auto kMonoMarkerAudio = std::to_array<std::uint8_t>({
      0x64,
      0x00, // frame 1: 16-bit sample 100
      0x65,
      0x00, // frame 2: 16-bit sample 101
    });
    constexpr auto kFloatMarkerAudio = std::to_array<std::uint8_t>({
      0x00,
      0x00,
      0x80,
      0x3F, // frame 1, channel 1: 1.0f
      0x00,
      0x00,
      0x80,
      0xBF, // frame 1, channel 2: -1.0f
      0x00,
      0x00,
      0x00,
      0x3F, // frame 2, channel 1: 0.5f
      0x00,
      0x00,
      0x00,
      0xBF, // frame 2, channel 2: -0.5f
    });

    std::uint32_t readLe32At(std::vector<std::uint8_t> const& bytes, std::size_t const offset) noexcept
    {
      return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
             (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
             (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
    }

    std::vector<std::uint8_t> makeAudioBytes(std::span<std::uint8_t const> const bytes)
    {
      return std::vector<std::uint8_t>{bytes.begin(), bytes.end()};
    }

    // Parse results for one file under both parse extents.
    struct ExtentParse final
    {
      Result<ParsedWave> completeRes{};
      Result<ParsedWave> requiredRes{};
    };

    // Parses the same bytes under both extents so a case can assert
    // preservation facts on each independently.
    ExtentParse parseBothExtents(std::vector<std::uint8_t> const& data)
    {
      auto const bytes = utility::bytes::view(data.data(), data.size());
      return ExtentParse{
        .completeRes = parseWave(bytes, WaveParseExtent::Complete),
        .requiredRes = parseWave(bytes, WaveParseExtent::RequiredAudio),
      };
    }

    // Standard extensible 16-bit PCM arrange: a complete wave with two marker
    // frames and the given raw mask written without validation. The caller
    // owns the returned bytes and must keep them alive while parsed spans are
    // read, so parseBothExtents stays a separate call at the use site.
    std::vector<std::uint8_t> makeExtensiblePcmWave(std::uint16_t const channels,
                                                    std::uint32_t const channelMask,
                                                    std::span<std::uint8_t const> const audio,
                                                    std::uint32_t const sampleRate = 48000)
    {
      return ao::test::wav::makeWav({
        .sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
        .channels = channels,
        .sampleRate = sampleRate,
        .bitsPerSample = 16,
        .channelMask = channelMask,
        .audioData = makeAudioBytes(audio),
      });
    }

    // The parsed audio span must equal the authored marker bytes and carry
    // whole frames only.
    void checkAudioSpan(ParsedWave const& parsed,
                        std::span<std::uint8_t const> const expectedAudio,
                        std::size_t const expectedFrameCount)
    {
      CHECK(parsed.data.size() == expectedAudio.size());
      REQUIRE(parsed.format.blockAlign > 0);
      CHECK((parsed.data.size() % parsed.format.blockAlign) == 0);
      CHECK((parsed.data.size() / parsed.format.blockAlign) == expectedFrameCount);
      CHECK(std::ranges::equal(parsed.data, utility::bytes::view(expectedAudio)));
    }

    // Expected preservation facts for one successfully parsed extent: the
    // declared layout, the raw mask, and the authored marker audio. Both
    // extents of one file must select the same declaration and audio range.
    struct PreservedWave final
    {
      std::uint16_t channels = 0;
      std::uint16_t blockAlign = 0;
      std::size_t dataOffset = 0;
      std::uint32_t channelMask = 0;
      std::span<std::uint8_t const> audio{};
      std::size_t frameCount = 0;
    };

    // One successfully parsed result must keep the declared layout, publish
    // the raw mask, and keep the authored marker audio.
    void checkPreservedWave(ParsedWave const& parsed, PreservedWave const& expected)
    {
      CHECK(parsed.format.channels == expected.channels);
      CHECK(parsed.format.blockAlign == expected.blockAlign);
      CHECK(parsed.dataOffset == expected.dataOffset);
      REQUIRE(parsed.format.optChannelMask);
      CHECK(*parsed.format.optChannelMask == expected.channelMask);
      checkAudioSpan(parsed, expected.audio, expected.frameCount);
    }
  } // namespace

  TEST_CASE("WAV RIFF parser - publishes no channel mask for legacy fmt chunks")
  {
    SECTION("PCM")
    {
      auto const data = ao::test::wav::makeWav({
        .sampleFormat = ao::test::wav::SampleFormat::Pcm,
        .channels = 2,
        .sampleRate = 48000,
        .bitsPerSample = 16,
        .channelMask = 0x3F,
        .audioData = makeAudioBytes(kStereoMarkerAudio),
      });
      auto extents = parseBothExtents(data);

      REQUIRE(extents.completeRes);
      CHECK(extents.completeRes->format.formatTag == kFormatPcm);
      CHECK(extents.completeRes->format.channels == 2);
      CHECK(extents.completeRes->format.blockAlign == 4);
      CHECK(extents.completeRes->dataOffset == 44);
      CHECK_FALSE(extents.completeRes->format.optChannelMask);
      checkAudioSpan(*extents.completeRes, kStereoMarkerAudio, 2);

      REQUIRE(extents.requiredRes);
      CHECK(extents.requiredRes->format.formatTag == kFormatPcm);
      CHECK(extents.requiredRes->format.channels == 2);
      CHECK_FALSE(extents.requiredRes->format.optChannelMask);
      checkAudioSpan(*extents.requiredRes, kStereoMarkerAudio, 2);

      // Arrange guard: a legacy fmt chunk is 16 bytes and has no channel
      // mask slot, so even a nonzero spec mask cannot create one.
      CHECK(readLe32At(data, kFmtChunkSizeFieldOffset) == 16);
    }

    SECTION("IEEE float")
    {
      auto const data = ao::test::wav::makeWav({
        .sampleFormat = ao::test::wav::SampleFormat::IeeeFloat,
        .channels = 2,
        .sampleRate = 48000,
        .bitsPerSample = 32,
        .channelMask = 0x3,
        .audioData = makeAudioBytes(kFloatMarkerAudio),
      });
      auto extents = parseBothExtents(data);

      REQUIRE(extents.completeRes);
      CHECK(extents.completeRes->format.formatTag == kFormatIeeeFloat);
      CHECK(extents.completeRes->format.isFloat);
      CHECK(extents.completeRes->format.channels == 2);
      CHECK(extents.completeRes->format.blockAlign == 8);
      CHECK(extents.completeRes->dataOffset == 44);
      CHECK_FALSE(extents.completeRes->format.optChannelMask);
      checkAudioSpan(*extents.completeRes, kFloatMarkerAudio, 2);

      REQUIRE(extents.requiredRes);
      CHECK(extents.requiredRes->format.formatTag == kFormatIeeeFloat);
      CHECK(extents.requiredRes->format.isFloat);
      CHECK(extents.requiredRes->format.channels == 2);
      CHECK_FALSE(extents.requiredRes->format.optChannelMask);
      checkAudioSpan(*extents.requiredRes, kFloatMarkerAudio, 2);

      // Arrange guard: a legacy fmt chunk is 16 bytes and has no channel
      // mask slot.
      CHECK(readLe32At(data, kFmtChunkSizeFieldOffset) == 16);
    }
  }

  TEST_CASE("WAV RIFF parser - stores an explicit zero channel mask as engaged zero")
  {
    auto const data = ao::test::wav::makeWav({
      .sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
      .channels = 2,
      .sampleRate = 48000,
      .bitsPerSample = 16,
      .channelMask = 0,
      .audioData = makeAudioBytes(kStereoMarkerAudio),
    });
    auto extents = parseBothExtents(data);

    // Arrange guard: the built fmt payload is the full 40-byte extensible
    // layout, so the parser faces a present mask slot holding zero rather
    // than a missing slot.
    CHECK(readLe32At(data, kFmtChunkSizeFieldOffset) == kExtensibleFmtPayloadSize);

    auto const expected = PreservedWave{
      .channels = 2,
      .blockAlign = 4,
      .dataOffset = 68,
      .channelMask = 0x0U,
      .audio = kStereoMarkerAudio,
      .frameCount = 2,
    };

    REQUIRE(extents.completeRes);
    checkPreservedWave(*extents.completeRes, expected);

    REQUIRE(extents.requiredRes);
    checkPreservedWave(*extents.requiredRes, expected);
  }

  TEST_CASE("WAV RIFF parser - preserves four-channel masks without normalization")
  {
    SECTION("quad front-left front-right back-left back-right")
    {
      auto const data = makeExtensiblePcmWave(4, 0x33U, kQuadMarkerAudio);
      auto extents = parseBothExtents(data);
      auto const expected = PreservedWave{
        .channels = 4,
        .blockAlign = 8,
        .dataOffset = 68,
        .channelMask = 0x33U,
        .audio = kQuadMarkerAudio,
        .frameCount = 2,
      };

      REQUIRE(extents.completeRes);
      checkPreservedWave(*extents.completeRes, expected);

      REQUIRE(extents.requiredRes);
      checkPreservedWave(*extents.requiredRes, expected);
    }

    SECTION("0x107 declares front-left front-right front-center back-center")
    {
      auto const data = makeExtensiblePcmWave(4, 0x107U, kQuadMarkerAudio);
      auto extents = parseBothExtents(data);
      auto const expected = PreservedWave{
        .channels = 4,
        .blockAlign = 8,
        .dataOffset = 68,
        .channelMask = 0x107U,
        .audio = kQuadMarkerAudio,
        .frameCount = 2,
      };

      REQUIRE(extents.completeRes);
      checkPreservedWave(*extents.completeRes, expected);

      REQUIRE(extents.requiredRes);
      checkPreservedWave(*extents.requiredRes, expected);
    }
  }

  TEST_CASE("WAV RIFF parser - preserves alternate speaker assignments")
  {
    SECTION("mono front-left instead of front-center")
    {
      auto const data = makeExtensiblePcmWave(1, 0x1U, kMonoMarkerAudio, 44100);
      auto extents = parseBothExtents(data);
      auto const expected = PreservedWave{
        .channels = 1,
        .blockAlign = 2,
        .dataOffset = 68,
        .channelMask = 0x1U,
        .audio = kMonoMarkerAudio,
        .frameCount = 2,
      };

      REQUIRE(extents.completeRes);
      checkPreservedWave(*extents.completeRes, expected);

      REQUIRE(extents.requiredRes);
      checkPreservedWave(*extents.requiredRes, expected);
    }

    SECTION("two speakers front-left and front-center")
    {
      auto const data = makeExtensiblePcmWave(2, 0x5U, kStereoMarkerAudio);
      auto extents = parseBothExtents(data);
      auto const expected = PreservedWave{
        .channels = 2,
        .blockAlign = 4,
        .dataOffset = 68,
        .channelMask = 0x5U,
        .audio = kStereoMarkerAudio,
        .frameCount = 2,
      };

      REQUIRE(extents.completeRes);
      checkPreservedWave(*extents.completeRes, expected);

      REQUIRE(extents.requiredRes);
      checkPreservedWave(*extents.requiredRes, expected);
    }
  }

  TEST_CASE("WAV RIFF parser - preserves raw channel masks without deriving channel count")
  {
    std::uint32_t channelMask = 0;

    SECTION("0x1 declares one speaker for two channels")
    {
      channelMask = 0x1;
    }

    SECTION("0x7 declares three speakers for two channels")
    {
      channelMask = 0x7;
    }

    SECTION("0x40000 sets a reserved speaker bit")
    {
      channelMask = 0x40000;
    }

    SECTION("0x80000000 sets the raw SPEAKER_ALL wildcard")
    {
      channelMask = 0x80000000U;
    }

    auto const data = makeExtensiblePcmWave(2, channelMask, kStereoMarkerAudio);
    auto extents = parseBothExtents(data);
    auto const expected = PreservedWave{
      .channels = 2,
      .blockAlign = 4,
      .dataOffset = 68,
      .channelMask = channelMask,
      .audio = kStereoMarkerAudio,
      .frameCount = 2,
    };

    REQUIRE(extents.completeRes);
    checkPreservedWave(*extents.completeRes, expected);

    REQUIRE(extents.requiredRes);
    checkPreservedWave(*extents.requiredRes, expected);
  }

  TEST_CASE("WAV RIFF parser - preserves channel mask for extensible IEEE float fmt")
  {
    auto const data = ao::test::wav::makeWav({
      .sampleFormat = ao::test::wav::SampleFormat::ExtensibleFloat,
      .channels = 2,
      .sampleRate = 48000,
      .bitsPerSample = 32,
      .channelMask = 0x3,
      .audioData = makeAudioBytes(kFloatMarkerAudio),
    });
    auto extents = parseBothExtents(data);
    auto const expected = PreservedWave{
      .channels = 2,
      .blockAlign = 8,
      .dataOffset = 68,
      .channelMask = 0x3U,
      .audio = kFloatMarkerAudio,
      .frameCount = 2,
    };

    REQUIRE(extents.completeRes);
    CHECK(extents.completeRes->format.formatTag == kFormatIeeeFloat);
    CHECK(extents.completeRes->format.isFloat);
    CHECK(extents.completeRes->format.validBitsPerSample == 32);
    checkPreservedWave(*extents.completeRes, expected);

    REQUIRE(extents.requiredRes);
    CHECK(extents.requiredRes->format.formatTag == kFormatIeeeFloat);
    CHECK(extents.requiredRes->format.isFloat);
    checkPreservedWave(*extents.requiredRes, expected);
  }

  TEST_CASE("WAV RIFF parser - RequiredAudio publishes mask and audio before malformed trailing chunks")
  {
    auto data = ao::test::wav::makeWav({
      .sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
      .channels = 2,
      .sampleRate = 48000,
      .bitsPerSample = 16,
      .channelMask = 0x3,
      .audioData = makeAudioBytes(kStereoMarkerAudio),
    });
    ao::test::wav::appendTruncatedChunk(data, "JUNK", 100);

    auto const bytes = utility::bytes::view(data.data(), data.size());
    auto requiredRes = parseWave(bytes, WaveParseExtent::RequiredAudio);
    auto completeRes = parseWave(bytes, WaveParseExtent::Complete);

    REQUIRE(requiredRes);
    CHECK(requiredRes->format.channels == 2);
    REQUIRE(requiredRes->format.optChannelMask);
    CHECK(*requiredRes->format.optChannelMask == 0x3U);
    checkAudioSpan(*requiredRes, kStereoMarkerAudio, 2);

    REQUIRE_FALSE(completeRes);
    CHECK(completeRes.error().code == Error::Code::CorruptData);
  }

  TEST_CASE("WAV RIFF parser - validates extensible fmt payload length and declared extension size")
  {
    // Known-good extensible fmt arrange: stereo, 48 kHz, 16-bit PCM, mask 0x3,
    // with two valid audio frames in the data chunk.
    auto const spec = ao::test::wav::Spec{
      .sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
      .channels = 2,
      .sampleRate = 48000,
      .bitsPerSample = 16,
      .channelMask = 0x3,
      .audioData = makeAudioBytes(kStereoMarkerAudio),
    };
    auto const fullFmt = ao::test::wav::makeFmtChunk(spec);
    REQUIRE(fullFmt.size() == kExtensibleFmtPayloadSize);

    constexpr auto kExtraFormatBytes = std::to_array<std::uint8_t>({0xA5, 0x5A});
    std::size_t fmtLength = 0;
    std::uint16_t declaredCbSize = 0;
    auto extraFormatBytes = std::span<std::uint8_t const>{kExtraFormatBytes}.first(0);
    std::size_t expectedDataOffset = 0;
    bool publishesMask = false;

    SECTION("complete payload with declared cbSize 22 publishes the mask")
    {
      fmtLength = kExtensibleFmtPayloadSize;
      declaredCbSize = 22;
      expectedDataOffset = 68;
      publishesMask = true;
    }

    SECTION("40-byte payload with declared cbSize 23 is rejected")
    {
      fmtLength = 40;
      declaredCbSize = 23;
    }

    SECTION("40-byte payload with declared cbSize 24 is rejected")
    {
      fmtLength = 40;
      declaredCbSize = 24;
    }

    SECTION("40-byte payload with declared cbSize 65535 is rejected")
    {
      fmtLength = 40;
      declaredCbSize = 65535;
    }

    SECTION("complete 23-byte extension preserves the declaration and audio")
    {
      fmtLength = 41;
      declaredCbSize = 23;
      extraFormatBytes = std::span<std::uint8_t const>{kExtraFormatBytes}.first(1);
      expectedDataOffset = 70;
      publishesMask = true;
    }

    SECTION("complete 24-byte extension preserves the declaration and audio")
    {
      fmtLength = 42;
      declaredCbSize = 24;
      extraFormatBytes = kExtraFormatBytes;
      expectedDataOffset = 70;
      publishesMask = true;
    }

    SECTION("RIFF alignment padding cannot supply a missing extension byte")
    {
      fmtLength = 41;
      declaredCbSize = 24;
      extraFormatBytes = std::span<std::uint8_t const>{kExtraFormatBytes}.first(1);
    }

    SECTION("complete base extension tolerates additional undeclared fmt bytes")
    {
      fmtLength = 42;
      declaredCbSize = 22;
      extraFormatBytes = kExtraFormatBytes;
      expectedDataOffset = 70;
      publishesMask = true;
    }

    SECTION("full payload with declared cbSize 0 is rejected")
    {
      fmtLength = kExtensibleFmtPayloadSize;
      declaredCbSize = 0;
    }

    SECTION("full payload with declared cbSize 20 is rejected")
    {
      fmtLength = kExtensibleFmtPayloadSize;
      declaredCbSize = 20;
    }

    SECTION("full payload with declared cbSize 21 is rejected")
    {
      fmtLength = kExtensibleFmtPayloadSize;
      declaredCbSize = 21;
    }

    SECTION("payload ends before the extension")
    {
      // The base fmt fields are complete; the cbSize, mask, and GUID slots
      // are all absent.
      fmtLength = 16;
      declaredCbSize = 22;
    }

    SECTION("payload ends before the channel mask slot")
    {
      // cbSize and valid bits are present, the mask slot is absent.
      fmtLength = 20;
      declaredCbSize = 22;
    }

    SECTION("channel mask half present")
    {
      // The mask slot holds its first two little-endian bytes.
      fmtLength = 22;
      declaredCbSize = 22;
    }

    SECTION("channel mask complete but subformat GUID absent")
    {
      // The mask slot is complete, the GUID slot is absent.
      fmtLength = 24;
      declaredCbSize = 22;
    }

    SECTION("subformat GUID incomplete")
    {
      // The GUID slot holds its first 15 bytes.
      fmtLength = 39;
      declaredCbSize = 22;
    }

    auto fmtPayload = fullFmt;
    fmtPayload.insert(fmtPayload.end(), extraFormatBytes.begin(), extraFormatBytes.end());
    // Every variant writes the declared extension size literally into fmt
    // payload bytes 16/17, so a rejection is attributable to the declared
    // size or the physical payload length, never to the outer RIFF framing
    // or the audio payload.
    fmtPayload[16] = static_cast<std::uint8_t>(declaredCbSize & 0xFFU);
    fmtPayload[17] = static_cast<std::uint8_t>((declaredCbSize >> 8U) & 0xFFU);
    REQUIRE(fmtLength <= fmtPayload.size());

    auto const data = ao::test::wav::makeWav(spec, std::span<std::uint8_t const>{fmtPayload}.first(fmtLength));
    // The outer chunk declares payload bytes only, including for the odd-sized
    // extension whose RIFF alignment byte cannot satisfy a cbSize overrun.
    REQUIRE(readLe32At(data, kFmtChunkSizeFieldOffset) == fmtLength);

    if (auto extents = parseBothExtents(data); publishesMask)
    {
      auto const expected = PreservedWave{
        .channels = 2,
        .blockAlign = 4,
        .dataOffset = expectedDataOffset,
        .channelMask = 0x3U,
        .audio = kStereoMarkerAudio,
        .frameCount = 2,
      };

      REQUIRE(extents.completeRes);
      checkPreservedWave(*extents.completeRes, expected);
      REQUIRE_FALSE(extents.completeRes->chunks.empty());
      CHECK(extents.completeRes->chunks.front().bytes.size() == fmtLength);

      REQUIRE(extents.requiredRes);
      checkPreservedWave(*extents.requiredRes, expected);
      REQUIRE_FALSE(extents.requiredRes->chunks.empty());
      CHECK(extents.requiredRes->chunks.front().bytes.size() == fmtLength);
    }
    else
    {
      REQUIRE_FALSE(extents.completeRes);
      CHECK(extents.completeRes.error().code == Error::Code::CorruptData);
      REQUIRE_FALSE(extents.requiredRes);
      CHECK(extents.requiredRes.error().code == Error::Code::CorruptData);
    }
  }
} // namespace ao::media::wav::test
