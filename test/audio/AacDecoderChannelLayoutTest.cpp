// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "DecoderTestSupport.h"
#include "lib/audio/AacDecoderSession.h"
#include "lib/audio/DecoderFactory.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/media/mp4/TestAtoms.h"
#include <ao/AudioCodec.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/media/mp4/Demuxer.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Raii.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fdk-aac/FDK_audio.h>
#include <fdk-aac/aacdecoder_lib.h>
#include <fdk-aac/machine_type.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    // Geometry of the retained aac-layout fixtures as measured by the pinned
    // FDK-AAC 2.0.3 harness: 13 raw packets, 1024 PCM frames per packet.
    constexpr std::uint32_t kAacFixtureSampleRate = 48000;
    constexpr std::uint64_t kAacFramesPerPacket = 1024;
    constexpr std::uint32_t kAacPacketCount = 13;
    constexpr std::uint64_t kAacTotalFrames = 13312;
    constexpr std::uint32_t kMdatPayloadOffset = 8;
    static_assert(sizeof(::INT_PCM) == sizeof(std::int16_t), "AAC native PCM must use 16-bit samples");

    struct NativeAacPcm final
    {
      std::uint32_t addedDelayFrames = 0;
      std::uint32_t sampleRate = 0;
      std::uint32_t framesPerPacket = 0;
      std::uint8_t channels = 0;
      std::vector<std::vector<std::int16_t>> packets;
    };

    struct AacLayoutCase final
    {
      std::string_view fixtureName;
      std::uint8_t channels;
      std::uint32_t speakerMask;
      ChannelLayoutOrigin origin;
      std::array<std::int16_t, 32> goldenWindow;
      std::size_t goldenPacketNonzeroSamples;
    };

    // Literal references from the independent x86-64 FDK-AAC 2.0.3 harness
    // (mapping 1, downmix and channel extension disabled), without Aobus code.
    // Cross-implementation windows allow one native s16 LSB; complete same-host
    // native PCM, adaptation and nonzero counts remain exact.
    constexpr auto kAacLayoutCases = std::to_array<AacLayoutCase>({
      {
        .fixtureName = "aac-layout/mono.m4a",
        .channels = 1,
        .speakerMask = 0x4,
        .origin = ChannelLayoutOrigin::Identified,
        .goldenWindow = {6, 5, 4, 5, 5, 4, 5, 6, 5, 4, 4, 4, 4, 4,  3,  2,
                         2, 3, 3, 2, 3, 4, 3, 2, 1, 0, 0, 1, 1, -2, -3, 0},
        .goldenPacketNonzeroSamples = 990,
      },
      {
        .fixtureName = "aac-layout/stereo.m4a",
        .channels = 2,
        .speakerMask = 0x3,
        .origin = ChannelLayoutOrigin::Identified,
        .goldenWindow = {16, 386, 18, 371, 22, 356, 25, 342, 26, 325, 26, 309, 26, 290, 22, 269,
                         20, 244, 23, 216, 25, 183, 24, 149, 25, 112, 29, 75,  28, 39,  26, 4},
        .goldenPacketNonzeroSamples = 2046,
      },
      {
        .fixtureName = "aac-layout/front3.m4a",
        .channels = 3,
        .speakerMask = 0x7,
        .origin = ChannelLayoutOrigin::Identified,
        .goldenWindow = {17, 146, -4, 18, 125, -3, 21, 107, -4, 24, 89, -5, 24, 73, -4, 24,
                         58, -3,  25, 44, -2,  22, 30, 0,   20, 15, 0,  24, -1, 0,  26, -18},
        .goldenPacketNonzeroSamples = 3039,
      },
      {
        .fixtureName = "aac-layout/back3.m4a",
        .channels = 3,
        .speakerMask = 0x103,
        .origin = ChannelLayoutOrigin::Identified,
        .goldenWindow = {16,  386, -5, 18,  371, -5, 22,  356, -4, 25,  342, -4, 26,  325, -3, 26,
                         309, -2,  26, 290, -2,  22, 269, 0,   20, 244, 1,   23, 216, 0,   25, 183},
        .goldenPacketNonzeroSamples = 3012,
      },
      {
        .fixtureName = "aac-layout/quad.m4a",
        .channels = 4,
        .speakerMask = 0x33,
        .origin = ChannelLayoutOrigin::Identified,
        .goldenWindow = {16, 386, 46, -85,  18, 371, 43, -101, 22, 356, 41, -115, 25, 342, 38, -129,
                         26, 325, 37, -140, 26, 309, 38, -145, 26, 290, 40, -150, 22, 269, 43, -152},
        .goldenPacketNonzeroSamples = 4091,
      },
      {
        .fixtureName = "aac-layout/back51.m4a",
        .channels = 6,
        .speakerMask = 0x3F,
        .origin = ChannelLayoutOrigin::Identified,
        .goldenWindow = {17, 146, -4, 54, 33, 49, 18, 125, -3, 53, 28, 27, 21, 107, -4, 53,
                         31, 7,   24, 89, -5, 52, 35, -19, 24, 73, -4, 51, 29, -44, 24, 58},
        .goldenPacketNonzeroSamples = 6097,
      },
      {
        .fixtureName = "aac-layout/surround71.m4a",
        .channels = 8,
        .speakerMask = 0x0,
        .origin = ChannelLayoutOrigin::Unknown,
        .goldenWindow = {17, 146, -4, 54, 33, 49, 16, -13, 18, 125, -3, 53, 28, 27,  30, -47,
                         21, 107, -4, 53, 31, 7,  43, -71, 24, 89,  -5, 52, 35, -19, 59, -96},
        .goldenPacketNonzeroSamples = 8143,
      },
      {
        .fixtureName = "aac-layout/wide71.m4a",
        .channels = 8,
        .speakerMask = 0x0,
        .origin = ChannelLayoutOrigin::Unknown,
        .goldenWindow = {17, 146, -4, 54, 33, 49, 16, -13, 18, 125, -3, 53, 28, 27,  30, -47,
                         21, 107, -4, 53, 31, 7,  43, -71, 24, 89,  -5, 52, 35, -19, 59, -96},
        .goldenPacketNonzeroSamples = 8143,
      },
    });

    std::vector<std::uint8_t> copyBytes(std::span<std::byte const> bytes)
    {
      auto const* const begin = utility::bytes::unsignedCharData(bytes);
      return {begin, begin + bytes.size()};
    }

    std::vector<std::int16_t> copySigned16LeSamples(std::span<std::byte const> bytes)
    {
      REQUIRE(bytes.size() % sizeof(std::int16_t) == 0);
      auto const sampleCount = bytes.size() / sizeof(std::int16_t);
      auto samples = std::vector<std::int16_t>(sampleCount);

      for (std::size_t index = 0; index < sampleCount; ++index)
      {
        samples[index] = readSigned16LePcmSample(bytes, index);
      }

      return samples;
    }

    NativeAacPcm decodeNativeFixture(std::string_view fixtureName, std::uint32_t firstPacketIndex = 0)
    {
      auto const fileData = readFileBytes(requireAudioFixture(fixtureName));
      auto const demuxerRes =
        media::mp4::Demuxer::parse(utility::bytes::view(std::span<std::uint8_t const>{fileData}), "mp4a");
      REQUIRE(demuxerRes);
      REQUIRE(demuxerRes->sampleCount() == kAacPacketCount);
      REQUIRE(firstPacketIndex < demuxerRes->sampleCount());

      // This oracle uses only the public native decoder API, without channel
      // interpretation, permutation, or Aobus decode/output helpers.
      auto decoderPtr = utility::makeUniquePtr<::aacDecoder_Close>(::aacDecoder_Open(TT_MP4_RAW, 1));
      REQUIRE(decoderPtr);
      REQUIRE(::aacDecoder_SetParam(decoderPtr.get(), AAC_PCM_OUTPUT_CHANNEL_MAPPING, 1) == AAC_DEC_OK);
      REQUIRE(::aacDecoder_SetParam(decoderPtr.get(), AAC_PCM_MAX_OUTPUT_CHANNELS, 0) == AAC_DEC_OK);
      REQUIRE(::aacDecoder_SetParam(decoderPtr.get(), AAC_PCM_MIN_OUTPUT_CHANNELS, 0) == AAC_DEC_OK);

      auto config = copyBytes(demuxerRes->magicCookie());
      REQUIRE_FALSE(config.empty());
      REQUIRE(std::in_range<::UINT>(config.size()));
      auto configData = std::array{config.data()};
      auto configSize = std::to_array<::UINT>({static_cast<::UINT>(config.size())});
      REQUIRE(::aacDecoder_ConfigRaw(decoderPtr.get(), configData.data(), configSize.data()) == AAC_DEC_OK);

      auto native = NativeAacPcm{};
      auto pcm = std::array<::INT_PCM, 16384>{};

      for (auto packetIndex = firstPacketIndex; packetIndex < demuxerRes->sampleCount(); ++packetIndex)
      {
        CAPTURE(packetIndex);
        auto input = copyBytes(demuxerRes->samplePayload(packetIndex));
        REQUIRE_FALSE(input.empty());
        REQUIRE(std::in_range<::UINT>(input.size()));
        auto inputData = std::array{input.data()};
        auto inputSize = std::to_array<::UINT>({static_cast<::UINT>(input.size())});
        auto bytesValid = inputSize[0];
        REQUIRE(::aacDecoder_Fill(decoderPtr.get(), inputData.data(), inputSize.data(), &bytesValid) == AAC_DEC_OK);
        REQUIRE(bytesValid == 0);
        REQUIRE(::aacDecoder_DecodeFrame(decoderPtr.get(), pcm.data(), static_cast<::INT>(pcm.size()), 0) ==
                AAC_DEC_OK);

        auto const* const info = ::aacDecoder_GetStreamInfo(decoderPtr.get());
        REQUIRE(info != nullptr);
        REQUIRE(info->sampleRate > 0);
        REQUIRE(info->frameSize > 0);
        REQUIRE(info->numChannels > 0);
        REQUIRE(std::in_range<std::uint8_t>(info->numChannels));
        auto const sampleCount =
          static_cast<std::size_t>(info->frameSize) * static_cast<std::size_t>(info->numChannels);
        REQUIRE(sampleCount <= pcm.size());

        if (native.packets.empty())
        {
          native.addedDelayFrames = info->outputDelay;
          native.sampleRate = static_cast<std::uint32_t>(info->sampleRate);
          native.framesPerPacket = static_cast<std::uint32_t>(info->frameSize);
          native.channels = static_cast<std::uint8_t>(info->numChannels);
        }

        REQUIRE(info->outputDelay == native.addedDelayFrames);
        REQUIRE(std::cmp_equal(info->sampleRate, native.sampleRate));
        REQUIRE(std::cmp_equal(info->frameSize, native.framesPerPacket));
        REQUIRE(std::cmp_equal(info->numChannels, native.channels));
        auto const samples = std::span<::INT_PCM const>{pcm}.first(sampleCount);
        native.packets.emplace_back(samples.begin(), samples.end());
      }

      return native;
    }

    void requireNativeFixtureGeometry(NativeAacPcm const& native,
                                      std::uint8_t channels,
                                      std::uint32_t firstPacketIndex = 0)
    {
      REQUIRE(native.sampleRate == kAacFixtureSampleRate);
      REQUIRE(native.framesPerPacket == kAacFramesPerPacket);
      REQUIRE(native.channels == channels);
      REQUIRE(native.packets.size() == kAacPacketCount - firstPacketIndex);
    }

    std::uint32_t goldenPacketIndex(NativeAacPcm const& native)
    {
      REQUIRE((native.addedDelayFrames == 720 || native.addedDelayFrames == 1744));
      // The known fixture window starts 304 frames after native added delay.
      // This test coordinate is not product delay or edit-list trimming.
      auto const goldenFrame = static_cast<std::uint64_t>(native.addedDelayFrames) + 304U;
      REQUIRE(goldenFrame % kAacFramesPerPacket == 0);
      auto const packetIndex = goldenFrame / kAacFramesPerPacket;
      REQUIRE(packetIndex < native.packets.size());
      return static_cast<std::uint32_t>(packetIndex);
    }

    std::size_t nonzeroSampleCount(std::vector<std::int16_t> const& samples)
    {
      return static_cast<std::size_t>(
        std::ranges::count_if(samples, [](std::int16_t const sample) { return sample != 0; }));
    }

    float readFloat32LePcmSample(std::span<std::byte const> bytes, std::size_t index)
    {
      REQUIRE(index < bytes.size() / sizeof(float));
      auto const offset = index * 4U;
      auto const bits = static_cast<std::uint32_t>(bytes[offset]) |
                        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
                        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
                        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
      return std::bit_cast<float>(bits);
    }

    void checkReadyInfo(DecodedStreamInfo const& info, AacLayoutCase const& testCase)
    {
      CHECK(info.codec == AudioCodec::Aac);
      CHECK(info.isLossy);
      CHECK(info.sourceFormat.sampleRate == kAacFixtureSampleRate);
      CHECK(info.sourceFormat.channels == testCase.channels);
      CHECK(info.sourceFormat.precisionBits == 16);
      CHECK(info.sourceFormat.sampleKind == SampleKind::Integer);
      CHECK(info.sourceFormat.speakerMask == testCase.speakerMask);
      CHECK(info.outputFormat.sampleRate == kAacFixtureSampleRate);
      CHECK(info.outputFormat.channels == testCase.channels);
      CHECK(info.outputFormat.speakerMask == testCase.speakerMask);
      CHECK(info.outputFormat.encoding == SampleEncoding::Signed16Le);
      CHECK(info.sourceChannelLayout.origin == testCase.origin);
      CHECK(info.duration > std::chrono::milliseconds{0});
    }

    std::vector<std::int16_t> readGoldenSigned16Samples(std::string_view fixtureName,
                                                        std::uint8_t channels,
                                                        std::uint32_t targetPacketIndex)
    {
      auto const testFile = requireAudioFixture(fixtureName);
      auto decoderPtr = ao::test::requireValue(AacDecoderSession::open(testFile, SampleEncoding::Signed16Le));
      auto& decoder = *decoderPtr;
      auto samples = std::vector<std::int16_t>{};

      for (std::uint32_t packetIndex = 0; packetIndex <= targetPacketIndex; ++packetIndex)
      {
        auto const blockRes = decoder.readNextBlock();
        REQUIRE(blockRes);
        REQUIRE(blockRes->frames == kAacFramesPerPacket);
        REQUIRE(blockRes->firstFrameIndex == static_cast<std::uint64_t>(packetIndex) * kAacFramesPerPacket);
        REQUIRE(blockRes->bytes.size() == static_cast<std::size_t>(blockRes->frames) * channels * sizeof(std::int16_t));

        if (packetIndex == targetPacketIndex)
        {
          samples = copySigned16LeSamples(blockRes->bytes);
        }
      }

      return samples;
    }

    bool isReferenceGoldenSampleWithinOneLsb(std::int16_t actual, std::int16_t expected) noexcept
    {
      // Widen before subtraction: the full int16 difference range fits int32.
      auto const delta = static_cast<std::int32_t>(actual) - static_cast<std::int32_t>(expected);
      return delta >= -1 && delta <= 1;
    }

    bool isAdaptedSampleEqual(SampleEncoding const encoding,
                              std::span<std::byte const> bytes,
                              std::size_t index,
                              std::int16_t baseSample)
    {
      switch (encoding)
      {
        case SampleEncoding::Signed24PackedLe:
          return readSigned24PackedLePcmSample(bytes, index) == static_cast<std::int64_t>(baseSample) * 256;
        case SampleEncoding::Signed24In32Le:
          return readSigned32LePcmSample(bytes, index) == static_cast<std::int64_t>(baseSample) * 256;
        case SampleEncoding::Signed32Le:
          return readSigned32LePcmSample(bytes, index) == static_cast<std::int64_t>(baseSample) * 65536;
        case SampleEncoding::Float32Le:
          return readFloat32LePcmSample(bytes, index) == static_cast<float>(baseSample) / 32768.0F;
        case SampleEncoding::Signed16Le:
        case SampleEncoding::Unknown: break;
      }

      return false;
    }
  } // namespace

  TEST_CASE("AacDecoderSession - ready info publishes the final native channel identity before the first read")
  {
    for (auto const& testCase : kAacLayoutCases)
    {
      CAPTURE(testCase.fixtureName);
      auto const testFile = requireAudioFixture(testCase.fixtureName);
      auto const native = decodeNativeFixture(testCase.fixtureName);
      requireNativeFixtureGeometry(native, testCase.channels);

      auto decoderPtr = ao::test::requireValue(AacDecoderSession::open(testFile, SampleEncoding::Signed16Le));
      auto& decoder = *decoderPtr;
      auto const info = decoder.streamInfo();
      checkReadyInfo(info, testCase);

      // Repeated inspection neither consumes nor mutates the primed first packet.
      CHECK(decoder.streamInfo() == info);
      CHECK(decoder.streamInfo() == info);

      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);
      CHECK(blockRes->frames == kAacFramesPerPacket);
      CHECK(blockRes->firstFrameIndex == 0);
      CHECK_FALSE(blockRes->endOfStream);
      auto const packet0 = copySigned16LeSamples(blockRes->bytes);
      CHECK(packet0 == native.packets[0]);

      // An explicit natural request and the factory publish the same identity.
      auto const nulloptPtr = ao::test::requireValue(AacDecoderSession::open(testFile, std::nullopt));
      CHECK(nulloptPtr->streamInfo() == info);
      auto const factoryPtr = ao::test::requireValue(openDecoderSession(testFile, std::nullopt));
      CHECK(factoryPtr->streamInfo() == info);
    }
  }

  TEST_CASE("AacDecoderSession - contiguous fixture packets match independent native PCM across the retained extent")
  {
    for (auto const& testCase : kAacLayoutCases)
    {
      CAPTURE(testCase.fixtureName);
      auto const testFile = requireAudioFixture(testCase.fixtureName);
      auto decoderPtr = ao::test::requireValue(AacDecoderSession::open(testFile, SampleEncoding::Signed16Le));
      auto& decoder = *decoderPtr;
      auto const info = decoder.streamInfo();
      auto const native = decodeNativeFixture(testCase.fixtureName);
      requireNativeFixtureGeometry(native, testCase.channels);

      std::uint64_t totalFrames = 0;

      for (std::uint32_t packetIndex = 0; packetIndex < kAacPacketCount; ++packetIndex)
      {
        CAPTURE(packetIndex);
        auto const blockRes = decoder.readNextBlock();
        REQUIRE(blockRes);
        CHECK(blockRes->frames == kAacFramesPerPacket);
        CHECK(blockRes->firstFrameIndex == static_cast<std::uint64_t>(packetIndex) * kAacFramesPerPacket);
        CHECK(blockRes->endOfStream == (packetIndex + 1U == kAacPacketCount));
        REQUIRE(blockRes->bytes.size() ==
                static_cast<std::size_t>(blockRes->frames) * testCase.channels * sizeof(std::int16_t));
        auto const samples = copySigned16LeSamples(blockRes->bytes);
        CHECK(samples == native.packets[packetIndex]);
        totalFrames += blockRes->frames;
        CHECK(decoder.streamInfo() == info);
      }

      CHECK(totalFrames == kAacTotalFrames);

      for (std::uint32_t repeat = 0; repeat < 2; ++repeat)
      {
        CAPTURE(repeat);
        auto const eofRes = decoder.readNextBlock();
        REQUIRE(eofRes);
        CHECK(eofRes->endOfStream);
        CHECK(eofRes->frames == 0);
        CHECK(eofRes->bytes.empty());
        CHECK(decoder.streamInfo() == info);
      }
    }
  }

  TEST_CASE("AacDecoderSession - native-delay-aware reference windows stay within one native signed-16 LSB")
  {
    for (auto const& testCase : kAacLayoutCases)
    {
      CAPTURE(testCase.fixtureName);
      auto const native = decodeNativeFixture(testCase.fixtureName);
      requireNativeFixtureGeometry(native, testCase.channels);
      auto const targetPacketIndex = goldenPacketIndex(native);
      CAPTURE(native.addedDelayFrames, targetPacketIndex);
      auto const testFile = requireAudioFixture(testCase.fixtureName);
      auto decoderPtr = ao::test::requireValue(AacDecoderSession::open(testFile, SampleEncoding::Signed16Le));
      auto& decoder = *decoderPtr;
      auto packet0 = std::vector<std::int16_t>{};
      auto goldenSamples = std::vector<std::int16_t>{};

      for (std::uint32_t packetIndex = 0; packetIndex <= targetPacketIndex; ++packetIndex)
      {
        CAPTURE(packetIndex);
        auto const blockRes = decoder.readNextBlock();
        REQUIRE(blockRes);
        CHECK(blockRes->frames == kAacFramesPerPacket);
        CHECK(blockRes->firstFrameIndex == static_cast<std::uint64_t>(packetIndex) * kAacFramesPerPacket);
        CHECK_FALSE(blockRes->endOfStream);
        REQUIRE(blockRes->bytes.size() ==
                static_cast<std::size_t>(kAacFramesPerPacket) * testCase.channels * sizeof(std::int16_t));

        if (packetIndex == 0)
        {
          packet0 = copySigned16LeSamples(blockRes->bytes);
        }

        if (packetIndex == targetPacketIndex)
        {
          goldenSamples = copySigned16LeSamples(blockRes->bytes);
        }
      }

      CHECK(packet0 == native.packets[0]);
      REQUIRE(goldenSamples.size() >= testCase.goldenWindow.size());

      for (std::size_t index = 0; index < testCase.goldenWindow.size(); ++index)
      {
        CAPTURE(index, goldenSamples[index], testCase.goldenWindow[index]);
        CHECK(isReferenceGoldenSampleWithinOneLsb(goldenSamples[index], testCase.goldenWindow[index]));
      }

      CHECK(nonzeroSampleCount(goldenSamples) == testCase.goldenPacketNonzeroSamples);
    }
  }

  TEST_CASE("AacDecoderSession - supported encodings adapt the native-delay-aware golden packet exactly from signed-16")
  {
    for (auto const& testCase : kAacLayoutCases)
    {
      CAPTURE(testCase.fixtureName);

      auto const native = decodeNativeFixture(testCase.fixtureName);
      requireNativeFixtureGeometry(native, testCase.channels);
      auto const targetPacketIndex = goldenPacketIndex(native);
      CAPTURE(native.addedDelayFrames, targetPacketIndex);

      // The s16 base matches complete same-host native PCM exactly and is also
      // bounded by the independent cross-implementation reference window.
      auto const base = readGoldenSigned16Samples(testCase.fixtureName, testCase.channels, targetPacketIndex);
      REQUIRE(base.size() == static_cast<std::size_t>(kAacFramesPerPacket) * testCase.channels);
      REQUIRE(base.size() >= testCase.goldenWindow.size());
      CHECK(base == native.packets[targetPacketIndex]);

      for (std::size_t index = 0; index < testCase.goldenWindow.size(); ++index)
      {
        CAPTURE(index, base[index], testCase.goldenWindow[index]);
        CHECK(isReferenceGoldenSampleWithinOneLsb(base[index], testCase.goldenWindow[index]));
      }

      CHECK(nonzeroSampleCount(base) == testCase.goldenPacketNonzeroSamples);

      for (auto const encoding : {SampleEncoding::Signed24PackedLe,
                                  SampleEncoding::Signed24In32Le,
                                  SampleEncoding::Signed32Le,
                                  SampleEncoding::Float32Le})
      {
        CAPTURE(sampleEncodingName(encoding));
        auto const testFile = requireAudioFixture(testCase.fixtureName);
        auto decoderPtr = ao::test::requireValue(AacDecoderSession::open(testFile, encoding));
        auto& decoder = *decoderPtr;

        for (std::uint32_t packetIndex = 0; packetIndex < targetPacketIndex; ++packetIndex)
        {
          auto const blockRes = decoder.readNextBlock();
          REQUIRE(blockRes);
          REQUIRE(blockRes->frames == kAacFramesPerPacket);
          REQUIRE(blockRes->firstFrameIndex == static_cast<std::uint64_t>(packetIndex) * kAacFramesPerPacket);
        }

        auto const goldenRes = decoder.readNextBlock();
        REQUIRE(goldenRes);
        CHECK(goldenRes->frames == kAacFramesPerPacket);
        CHECK(goldenRes->firstFrameIndex == static_cast<std::uint64_t>(targetPacketIndex) * kAacFramesPerPacket);
        REQUIRE(goldenRes->bytes.size() == base.size() * bytesPerSample(encoding));

        auto optMismatchIndex = std::optional<std::size_t>{};

        for (std::size_t index = 0; index < base.size(); ++index)
        {
          if (!isAdaptedSampleEqual(encoding, goldenRes->bytes, index, base[index]))
          {
            optMismatchIndex = index;
            break;
          }
        }

        if (optMismatchIndex)
        {
          CAPTURE(*optMismatchIndex, base[*optMismatchIndex]);
        }

        CHECK_FALSE(optMismatchIndex);
      }
    }
  }

  TEST_CASE("AacDecoderSession - flush before the first read keeps the pending primed packet")
  {
    auto const native = decodeNativeFixture("aac-layout/stereo.m4a");
    requireNativeFixtureGeometry(native, 2);
    auto const testFile = requireAudioFixture("aac-layout/stereo.m4a");
    auto decoderPtr = ao::test::requireValue(AacDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;
    auto const info = decoder.streamInfo();

    decoder.flush();
    decoder.flush();
    CHECK(decoder.streamInfo() == info);

    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK(blockRes->frames == kAacFramesPerPacket);
    CHECK(blockRes->firstFrameIndex == 0);
    CHECK_FALSE(blockRes->endOfStream);
    auto const packet0 = copySigned16LeSamples(blockRes->bytes);
    CHECK(packet0 == native.packets[0]);

    // The pending packet was returned, not skipped: the next read continues
    // with packet 1.
    auto const nextRes = decoder.readNextBlock();
    REQUIRE(nextRes);
    CHECK(nextRes->frames == kAacFramesPerPacket);
    CHECK(nextRes->firstFrameIndex == kAacFramesPerPacket);
    CHECK(copySigned16LeSamples(nextRes->bytes) == native.packets[1]);
    CHECK(decoder.streamInfo() == info);
  }

  TEST_CASE("AacDecoderSession - initial seek covering the first packet returns the pristine primed block")
  {
    auto const native = decodeNativeFixture("aac-layout/stereo.m4a");
    requireNativeFixtureGeometry(native, 2);
    auto const testFile = requireAudioFixture("aac-layout/stereo.m4a");
    auto decoderPtr = ao::test::requireValue(AacDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;
    auto const info = decoder.streamInfo();

    SECTION("Seek to the start")
    {
      REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
      CHECK(decoder.streamInfo() == info);

      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);
      CHECK(blockRes->frames == kAacFramesPerPacket);
      CHECK(blockRes->firstFrameIndex == 0);
      CHECK_FALSE(blockRes->endOfStream);
      auto const packet0 = copySigned16LeSamples(blockRes->bytes);
      CHECK(packet0 == native.packets[0]);

      auto const nextRes = decoder.readNextBlock();
      REQUIRE(nextRes);
      CHECK(nextRes->firstFrameIndex == kAacFramesPerPacket);
      CHECK(copySigned16LeSamples(nextRes->bytes) == native.packets[1]);
    }

    SECTION("Seek within the first packet")
    {
      auto const offset = std::chrono::milliseconds{5};
      auto const targetFrame = static_cast<std::uint64_t>(offset.count()) * kAacFixtureSampleRate / 1000U;

      REQUIRE(decoder.seek(offset));
      CHECK(decoder.streamInfo() == info);

      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);
      CHECK(blockRes->frames == kAacFramesPerPacket);
      CHECK(blockRes->firstFrameIndex <= targetFrame);
      CHECK(blockRes->firstFrameIndex + blockRes->frames > targetFrame);
      CHECK_FALSE(blockRes->endOfStream);
      auto const packet0 = copySigned16LeSamples(blockRes->bytes);
      CHECK(packet0 == native.packets[0]);

      auto const nextRes = decoder.readNextBlock();
      REQUIRE(nextRes);
      CHECK(nextRes->firstFrameIndex == kAacFramesPerPacket);
      CHECK(copySigned16LeSamples(nextRes->bytes) == native.packets[1]);
    }
  }

  TEST_CASE("AacDecoderSession - initial mid-stream seek matches a fresh native decode from the target packet")
  {
    auto const testFile = requireAudioFixture("aac-layout/stereo.m4a");
    auto decoderPtr = ao::test::requireValue(AacDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;
    auto const info = decoder.streamInfo();

    auto const native = decodeNativeFixture("aac-layout/stereo.m4a", 4);
    requireNativeFixtureGeometry(native, 2, 4);

    constexpr auto kOffset = std::chrono::milliseconds{100};
    auto const targetFrame = static_cast<std::uint64_t>(kOffset.count()) * kAacFixtureSampleRate / 1000U;

    REQUIRE(decoder.seek(kOffset));
    CHECK(decoder.streamInfo() == info);

    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK(blockRes->frames == kAacFramesPerPacket);
    CHECK(blockRes->firstFrameIndex == 4U * kAacFramesPerPacket);
    CHECK(blockRes->firstFrameIndex <= targetFrame);
    CHECK(blockRes->firstFrameIndex + blockRes->frames > targetFrame);
    CHECK_FALSE(blockRes->endOfStream);

    // Compare the entire packet against a fresh native handle starting at raw
    // packet 4, without assuming that cold-start PCM is universally silent.
    REQUIRE(blockRes->bytes.size() == 4096);
    auto const packet4 = copySigned16LeSamples(blockRes->bytes);
    CHECK(packet4 == native.packets[0]);

    auto const nextRes = decoder.readNextBlock();
    REQUIRE(nextRes);
    CHECK(nextRes->frames == kAacFramesPerPacket);
    CHECK(nextRes->firstFrameIndex == blockRes->firstFrameIndex + kAacFramesPerPacket);
    CHECK(copySigned16LeSamples(nextRes->bytes) == native.packets[1]);
    CHECK(decoder.streamInfo() == info);
  }

  TEST_CASE("AacDecoderSession - seeking back to the start after end of stream retains the published identity")
  {
    auto const testFile = requireAudioFixture("aac-layout/stereo.m4a");
    auto decoderPtr = ao::test::requireValue(AacDecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;
    auto const info = decoder.streamInfo();

    CHECK(readUntilStableEndOfStream(decoder, 32) == kAacTotalFrames);
    CHECK(decoder.streamInfo() == info);

    REQUIRE(decoder.seek(std::chrono::milliseconds{0}));

    // Packet 0 is re-decoded on the primed handle whose filter history already
    // advanced past it, so only the packet extent and identity are pinned here.
    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK(blockRes->frames == kAacFramesPerPacket);
    CHECK(blockRes->firstFrameIndex == 0);
    CHECK_FALSE(blockRes->endOfStream);
    CHECK(decoder.streamInfo() == info);
  }

  TEST_CASE("AacDecoderSession - a single-sample MP4 reports end of stream with its primed block")
  {
    // A real first packet and cookie from the retained stereo fixture, wrapped
    // in a one-sample MP4 table.
    auto const fixturePath = requireAudioFixture("aac-layout/stereo.m4a");
    auto const fileData = readFileBytes(fixturePath);
    auto const demuxerRes =
      media::mp4::Demuxer::parse(utility::bytes::view(std::span<std::uint8_t const>{fileData}), "mp4a");
    REQUIRE(demuxerRes);
    REQUIRE(demuxerRes->sampleCount() >= 1);

    auto const cookie = copyBytes(demuxerRes->magicCookie());
    REQUIRE_FALSE(cookie.empty());

    auto const firstPacket = copyBytes(demuxerRes->samplePayload(0));
    REQUIRE_FALSE(firstPacket.empty());

    auto esdsBody = std::vector<std::uint8_t>{0, 0, 0, 0};
    esdsBody.push_back(0x05);
    esdsBody.push_back(static_cast<std::uint8_t>(cookie.size()));
    esdsBody.insert(esdsBody.end(), cookie.begin(), cookie.end());

    auto data = std::vector<std::uint8_t>{};
    ao::test::mp4::addAtom(data, "mdat", firstPacket);
    auto const track = ao::test::mp4::makeCompleteAudioTrackAtom("mp4a",
                                                                 ao::test::mp4::makeAtom("esds", esdsBody),
                                                                 kAacFixtureSampleRate,
                                                                 kAacFramesPerPacket,
                                                                 static_cast<std::uint32_t>(firstPacket.size()),
                                                                 kAacFramesPerPacket,
                                                                 kMdatPayloadOffset);
    auto const moov = ao::test::mp4::makeAtom("moov", track);
    data.insert(data.end(), moov.begin(), moov.end());

    auto const temp = ao::test::TempFile{data, ".m4a"};
    auto decoderPtr = ao::test::requireValue(AacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;
    auto const info = decoder.streamInfo();
    CHECK(info.codec == AudioCodec::Aac);
    CHECK(info.sourceFormat.sampleRate == kAacFixtureSampleRate);
    CHECK(info.sourceFormat.channels == 2);
    CHECK(info.sourceFormat.speakerMask == 0x3);
    CHECK(info.sourceChannelLayout.origin == ChannelLayoutOrigin::Identified);

    // The cached single block is nonempty and already reports end of stream.
    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK(blockRes->frames == kAacFramesPerPacket);
    CHECK(blockRes->firstFrameIndex == 0);
    CHECK(blockRes->endOfStream);
    REQUIRE(blockRes->bytes.size() ==
            static_cast<std::size_t>(blockRes->frames) * info.outputFormat.channels * sizeof(std::int16_t));

    for (std::uint32_t repeat = 0; repeat < 2; ++repeat)
    {
      auto const eofRes = decoder.readNextBlock();
      REQUIRE(eofRes);
      CHECK(eofRes->endOfStream);
      CHECK(eofRes->frames == 0);
      CHECK(eofRes->bytes.empty());
    }

    CHECK(decoder.streamInfo() == info);
  }
} // namespace ao::audio::test
