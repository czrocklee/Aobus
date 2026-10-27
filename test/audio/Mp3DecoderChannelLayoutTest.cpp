// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/DecoderFactory.h"
#include "lib/audio/Mp3DecoderSession.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    constexpr std::size_t kLayerIiiByteCount = 417;
    constexpr std::size_t kLayerIByteCount = 136;
    constexpr std::size_t kLayerIii48kByteCount = 384;
    constexpr std::size_t kId3v1ByteCount = 128;
    constexpr std::uint32_t kSampleRate44100 = 44100;
    constexpr std::uint64_t kLayerIiiPcmFrames = 1152;
    constexpr std::uint64_t kLayerIPcmFrames = 384;
    constexpr std::uint64_t kGoodPrefixFrames = 3U * kLayerIiiPcmFrames;
    constexpr std::size_t kIndexProbeFrameCount = 16385;

    // Native mpg123 1.33.7 probe: synthetic MPEG1 Layer III frames of 417
    // bytes at 128 kbit/s and 44100 Hz without CRC decode to 1152 zero-valued
    // S16 PCM frames. MPEG1 Layer I frames of 136 bytes at the same rate and
    // bitrate decode to 384 PCM frames. The two mode bits select one frame's
    // channel arrangement; published identity requires every indexed frame to
    // share one known role.
    enum class Mp3ChannelMode : std::uint8_t
    {
      Stereo = 0,
      JointStereo = 1,
      DualChannel = 2,
      Mono = 3,
    };

    enum class MpegFrameKind : std::uint8_t
    {
      LayerIii44100,
      LayerI44100,
      LayerIii48000,
    };

    // Independent literal expectations per mode; 0x3 assigns FL|FR and 0x4
    // assigns FC in WAVE speaker bits. Dual channel carries two independent
    // mono programs, not an assigned FL/FR pair, so it publishes no layout.
    struct ModeCase final
    {
      Mp3ChannelMode mode;
      std::uint8_t channels = 0;
      std::uint32_t speakerMask = 0;
      ChannelLayoutOrigin origin = ChannelLayoutOrigin::Unknown;
    };

    constexpr auto kStereoCase = ModeCase{.mode = Mp3ChannelMode::Stereo,
                                          .channels = 2,
                                          .speakerMask = 0x3,
                                          .origin = ChannelLayoutOrigin::Identified};
    constexpr auto kJointCase = ModeCase{.mode = Mp3ChannelMode::JointStereo,
                                         .channels = 2,
                                         .speakerMask = 0x3,
                                         .origin = ChannelLayoutOrigin::Identified};
    constexpr auto kDualCase = ModeCase{.mode = Mp3ChannelMode::DualChannel,
                                        .channels = 2,
                                        .speakerMask = 0,
                                        .origin = ChannelLayoutOrigin::Unknown};
    constexpr auto kMonoCase = ModeCase{.mode = Mp3ChannelMode::Mono,
                                        .channels = 1,
                                        .speakerMask = 0x4,
                                        .origin = ChannelLayoutOrigin::Identified};

    constexpr auto kModeCases = std::to_array<ModeCase>({kStereoCase, kJointCase, kDualCase, kMonoCase});

    struct MpegFrameSpec final
    {
      Mp3ChannelMode mode = Mp3ChannelMode::Stereo;
      MpegFrameKind kind = MpegFrameKind::LayerIii44100;
    };

    struct DriftStream final
    {
      ModeCase initialCase;
      std::vector<Mp3ChannelMode> frameModes;
    };

    struct GeometryStream final
    {
      std::uint8_t channels = 0;
      std::uint32_t sampleRate = kSampleRate44100;
      std::optional<std::chrono::milliseconds> optDuration = {};
      std::vector<MpegFrameSpec> frames;
    };

    struct ConcreteAndFactory final
    {
      std::unique_ptr<Mp3DecoderSession> concretePtr;
      std::unique_ptr<DecoderSession> factoryPtr;
    };

    std::vector<std::uint8_t> buildMpegFrameBytes(MpegFrameSpec const& spec)
    {
      std::size_t byteCount = kLayerIiiByteCount;
      std::uint8_t versionLayer = 0xFB;
      std::uint8_t bitrateRate = 0x90;

      switch (spec.kind)
      {
        case MpegFrameKind::LayerIii44100: break;
        case MpegFrameKind::LayerI44100:
          byteCount = kLayerIByteCount;
          versionLayer = 0xFF;
          bitrateRate = 0x40;
          break;
        case MpegFrameKind::LayerIii48000:
          byteCount = kLayerIii48kByteCount;
          versionLayer = 0xFB;
          bitrateRate = 0x94;
          break;
      }

      auto frame = std::vector<std::uint8_t>(byteCount, 0U);
      frame[0] = 0xFF;
      frame[1] = versionLayer;
      frame[2] = bitrateRate;
      frame[3] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(spec.mode) << 6U);

      return frame;
    }

    std::vector<std::uint8_t> buildMpegFrameBytes(Mp3ChannelMode const mode)
    {
      return buildMpegFrameBytes(MpegFrameSpec{.mode = mode});
    }

    std::vector<std::uint8_t> buildFrameStream(std::span<MpegFrameSpec const> const frames)
    {
      auto data = std::vector<std::uint8_t>{};

      for (MpegFrameSpec const& spec : frames)
      {
        auto const frame = buildMpegFrameBytes(spec);
        data.insert(data.end(), frame.begin(), frame.end());
      }

      return data;
    }

    std::vector<std::uint8_t> buildModeStream(std::span<Mp3ChannelMode const> const modes)
    {
      auto data = std::vector<std::uint8_t>{};

      for (Mp3ChannelMode const mode : modes)
      {
        auto const frame = buildMpegFrameBytes(mode);
        data.insert(data.end(), frame.begin(), frame.end());
      }

      return data;
    }

    std::vector<std::uint8_t> buildConstantModeStream(Mp3ChannelMode const mode, std::size_t const frameCount)
    {
      auto const modes = std::vector<Mp3ChannelMode>(frameCount, mode);

      return buildModeStream(modes);
    }

    std::vector<std::uint8_t> buildModeRun(Mp3ChannelMode const first,
                                           std::size_t const firstCount,
                                           Mp3ChannelMode const second,
                                           std::size_t const secondCount,
                                           Mp3ChannelMode const third = Mp3ChannelMode::Stereo,
                                           std::size_t const thirdCount = 0)
    {
      auto modes = std::vector<Mp3ChannelMode>(firstCount, first);
      modes.insert(modes.end(), secondCount, second);
      modes.insert(modes.end(), thirdCount, third);

      return buildModeStream(modes);
    }

    std::vector<std::uint8_t> buildLayerTransitionStream()
    {
      auto frames = std::vector<MpegFrameSpec>(5, MpegFrameSpec{.kind = MpegFrameKind::LayerI44100});
      frames.insert(frames.end(), 5, MpegFrameSpec{.kind = MpegFrameKind::LayerIii44100});

      return buildFrameStream(frames);
    }

    void appendId3v1Tag(std::vector<std::uint8_t>& data)
    {
      auto const tagOffset = data.size();
      data.resize(tagOffset + kId3v1ByteCount, 0U);
      data[tagOffset] = static_cast<std::uint8_t>('T');
      data[tagOffset + 1] = static_cast<std::uint8_t>('A');
      data[tagOffset + 2] = static_cast<std::uint8_t>('G');
    }

    ConcreteAndFactory openConcreteAndFactory(std::filesystem::path const& path,
                                              std::optional<SampleEncoding> const optEncoding)
    {
      return ConcreteAndFactory{.concretePtr = ao::test::requireValue(Mp3DecoderSession::open(path, optEncoding)),
                                .factoryPtr = ao::test::requireValue(openDecoderSession(path, optEncoding))};
    }

    void checkModeIdentity(DecodedStreamInfo const& info, ModeCase const& expected, SampleEncoding const encoding)
    {
      CHECK(info.codec == AudioCodec::Mp3);
      CHECK(info.isLossy);
      CHECK(info.sourceFormat == SignalFormat{.sampleRate = kSampleRate44100,
                                              .channels = expected.channels,
                                              .precisionBits = 16,
                                              .sampleKind = SampleKind::Integer,
                                              .speakerMask = expected.speakerMask});
      CHECK(info.outputFormat == PcmFormat{.sampleRate = kSampleRate44100,
                                           .channels = expected.channels,
                                           .encoding = encoding,
                                           .speakerMask = expected.speakerMask});
      CHECK(info.sourceChannelLayout.origin == expected.origin);
      CHECK_FALSE(info.optTotalFrames);
    }

    void checkWithheldIdentity(DecodedStreamInfo const& info,
                               std::uint8_t const channels,
                               std::uint32_t const sampleRate,
                               SampleEncoding const encoding)
    {
      CHECK(info.codec == AudioCodec::Mp3);
      CHECK(info.isLossy);
      CHECK(info.sourceFormat == SignalFormat{.sampleRate = sampleRate,
                                              .channels = channels,
                                              .precisionBits = 16,
                                              .sampleKind = SampleKind::Integer,
                                              .speakerMask = 0});
      CHECK(info.outputFormat ==
            PcmFormat{.sampleRate = sampleRate, .channels = channels, .encoding = encoding, .speakerMask = 0});
      CHECK(info.sourceChannelLayout.origin == ChannelLayoutOrigin::Unknown);
      CHECK_FALSE(info.optTotalFrames);
    }

    void checkSilentBlock(PcmBlock const& block,
                          std::uint64_t const expectedFirstFrameIndex,
                          std::uint8_t const channels,
                          std::uint32_t const sampleBytes)
    {
      REQUIRE(block.frames > 0);
      CHECK(block.firstFrameIndex == expectedFirstFrameIndex);
      REQUIRE(block.bytes.size() ==
              static_cast<std::size_t>(block.frames) * static_cast<std::size_t>(channels) * sampleBytes);
      CHECK(std::ranges::all_of(block.bytes, [](std::byte const val) { return val == std::byte{0}; }));
    }

    void expectStableEndOfStream(DecoderSession& decoder, DecodedStreamInfo const& expectedInfo)
    {
      for (std::uint32_t repeat = 0; repeat < 2U; ++repeat)
      {
        auto const endRes = decoder.readNextBlock();
        REQUIRE(endRes);
        CHECK(endRes->endOfStream);
        CHECK(endRes->frames == 0);
        CHECK(endRes->bytes.empty());
        CHECK(decoder.streamInfo() == expectedInfo);
      }
    }

    void decodeSilentRange(DecoderSession& decoder,
                           std::uint64_t const firstFrameIndex,
                           std::uint64_t const endFrameIndex,
                           DecodedStreamInfo const& expectedInfo)
    {
      auto const channels = expectedInfo.outputFormat.channels;
      auto const sampleBytes = bytesPerSample(expectedInfo.outputFormat.encoding);
      std::uint64_t totalFrames = firstFrameIndex;

      while (totalFrames < endFrameIndex)
      {
        auto const blockRes = decoder.readNextBlock();
        REQUIRE(blockRes);
        REQUIRE_FALSE(blockRes->endOfStream);
        REQUIRE(totalFrames + blockRes->frames <= endFrameIndex);
        checkSilentBlock(*blockRes, totalFrames, channels, sampleBytes);
        totalFrames += blockRes->frames;
        CHECK(decoder.streamInfo() == expectedInfo);
      }

      CHECK(totalFrames == endFrameIndex);
    }

    void decodeSilentStream(DecoderSession& decoder, std::uint64_t const expectedTotalFrames)
    {
      auto const info = decoder.streamInfo();
      decodeSilentRange(decoder, 0, expectedTotalFrames, info);
      expectStableEndOfStream(decoder, info);
    }

    // Delivers only the silent good prefix of a drifting stream and asserts
    // that the first read past it is rejected as NotSupported. No delivered
    // block may extend beyond the known good prefix.
    std::uint64_t consumeSilentPrefixUntilDrift(DecoderSession& decoder, std::uint64_t const goodPrefixFrames)
    {
      auto const info = decoder.streamInfo();
      auto const channels = info.outputFormat.channels;
      auto const sampleBytes = bytesPerSample(info.outputFormat.encoding);
      std::uint64_t totalFrames = 0;

      while (totalFrames < goodPrefixFrames)
      {
        auto const blockRes = decoder.readNextBlock();

        if (!blockRes)
        {
          CHECK(blockRes.error().code == Error::Code::NotSupported);
          CHECK(decoder.streamInfo() == info);

          return totalFrames;
        }

        REQUIRE(totalFrames + blockRes->frames <= goodPrefixFrames);
        checkSilentBlock(*blockRes, totalFrames, channels, sampleBytes);
        totalFrames += blockRes->frames;
        CHECK(decoder.streamInfo() == info);
      }

      auto const driftRes = decoder.readNextBlock();
      REQUIRE_FALSE(driftRes);
      CHECK(driftRes.error().code == Error::Code::NotSupported);
      CHECK(decoder.streamInfo() == info);

      return totalFrames;
    }

    void checkPersistentNotSupported(DecoderSession& decoder, DecodedStreamInfo const& expectedInfo)
    {
      for (std::uint32_t repeat = 0; repeat < 2U; ++repeat)
      {
        auto const repeatRes = decoder.readNextBlock();
        REQUIRE_FALSE(repeatRes);
        CHECK(repeatRes.error().code == Error::Code::NotSupported);
        CHECK(decoder.streamInfo() == expectedInfo);
      }

      decoder.flush();
      auto const flushedRes = decoder.readNextBlock();
      REQUIRE_FALSE(flushedRes);
      CHECK(flushedRes.error().code == Error::Code::NotSupported);
      CHECK(decoder.streamInfo() == expectedInfo);
    }

    void checkSeekRefusesChangedGeometry(DecoderSession& decoder,
                                         std::chrono::milliseconds const offset,
                                         DecodedStreamInfo const& expectedInfo)
    {
      REQUIRE(decoder.seek(offset));
      CHECK(decoder.streamInfo() == expectedInfo);
      auto const refusedRes = decoder.readNextBlock();
      REQUIRE_FALSE(refusedRes);
      CHECK(refusedRes.error().code == Error::Code::NotSupported);
      CHECK(decoder.streamInfo() == expectedInfo);
    }

    void decodeLayerTransition(DecoderSession& decoder, DecodedStreamInfo const& expectedInfo)
    {
      constexpr auto kBlockFrames = std::to_array<std::uint64_t>({kLayerIPcmFrames,
                                                                  kLayerIPcmFrames,
                                                                  kLayerIPcmFrames,
                                                                  kLayerIPcmFrames,
                                                                  kLayerIPcmFrames,
                                                                  kLayerIiiPcmFrames,
                                                                  kLayerIiiPcmFrames,
                                                                  kLayerIiiPcmFrames,
                                                                  kLayerIiiPcmFrames,
                                                                  kLayerIiiPcmFrames});
      auto const channels = expectedInfo.outputFormat.channels;
      auto const sampleBytes = bytesPerSample(expectedInfo.outputFormat.encoding);
      std::uint64_t totalFrames = 0;

      for (std::uint64_t const blockFrames : kBlockFrames)
      {
        auto const blockRes = decoder.readNextBlock();
        REQUIRE(blockRes);
        REQUIRE(blockRes->frames == blockFrames);
        checkSilentBlock(*blockRes, totalFrames, channels, sampleBytes);
        totalFrames += blockRes->frames;
        CHECK(decoder.streamInfo() == expectedInfo);
      }

      CHECK(totalFrames == 7680);
      expectStableEndOfStream(decoder, expectedInfo);
    }
  } // namespace

  TEST_CASE("Mp3DecoderSession - frame mode publishes mono and stereo identities and withholds dual")
  {
    for (ModeCase const& testCase : kModeCases)
    {
      CAPTURE(static_cast<int>(testCase.mode));
      auto const temp = ao::test::TempFile{buildConstantModeStream(testCase.mode, 5), ".mp3"};
      auto opened = openConcreteAndFactory(temp.path, std::nullopt);
      auto const info = opened.concretePtr->streamInfo();
      checkModeIdentity(info, testCase, SampleEncoding::Signed16Le);

      // Five fixture frames decode to 5760 PCM frames; the scanned length
      // floors to 130 ms at 44100 Hz.
      CHECK(info.duration == std::chrono::milliseconds{130});
      CHECK(opened.factoryPtr->streamInfo() == info);
      decodeSilentStream(*opened.concretePtr, 5U * kLayerIiiPcmFrames);
      decodeSilentStream(*opened.factoryPtr, 5U * kLayerIiiPcmFrames);
    }
  }

  TEST_CASE("Mp3DecoderSession - encoded Xing and VBR fixtures retain identified roles")
  {
    auto const* const fixtureName = GENERATE("basic_metadata.mp3", "hires.mp3", "vbr_no_seek_table.mp3");
    CAPTURE(fixtureName);
    auto const path = requireAudioFixture(fixtureName);
    auto opened = openConcreteAndFactory(path, std::nullopt);
    auto const info = opened.concretePtr->streamInfo();
    // The retained VBR fixture was authored from mono sine; the other two fixtures are stereo.
    std::uint32_t const expectedMask = std::string_view{fixtureName} == "vbr_no_seek_table.mp3" ? 0x4U : 0x3U;
    CHECK(info.sourceFormat.speakerMask == expectedMask);
    CHECK(info.outputFormat.speakerMask == expectedMask);
    CHECK(info.sourceChannelLayout.origin == ChannelLayoutOrigin::Identified);
    CHECK(opened.factoryPtr->streamInfo() == info);
    auto const firstBlockRes = opened.concretePtr->readNextBlock();
    REQUIRE(firstBlockRes);
    CHECK(firstBlockRes->firstFrameIndex == 0U);
    CHECK(firstBlockRes->frames > 0U);
    CHECK(opened.concretePtr->streamInfo() == info);
  }

  TEST_CASE("Mp3DecoderSession - integer and float carriers adapt silence without changing the published layout")
  {
    auto const encoding = GENERATE(SampleEncoding::Signed24PackedLe,
                                   SampleEncoding::Signed24In32Le,
                                   SampleEncoding::Signed32Le,
                                   SampleEncoding::Float32Le);

    for (ModeCase const& testCase : kModeCases)
    {
      CAPTURE(static_cast<int>(testCase.mode), static_cast<int>(encoding));
      auto const temp = ao::test::TempFile{buildConstantModeStream(testCase.mode, 5), ".mp3"};
      auto opened = openConcreteAndFactory(temp.path, encoding);
      checkModeIdentity(opened.concretePtr->streamInfo(), testCase, encoding);
      CHECK(opened.factoryPtr->streamInfo() == opened.concretePtr->streamInfo());

      // Literal zero PCM stays zero in every adapted container, float included.
      decodeSilentStream(*opened.concretePtr, 5U * kLayerIiiPcmFrames);
    }
  }

  TEST_CASE("Mp3DecoderSession - a zero seek restores the whole silent stream without lost or doubled frames")
  {
    for (ModeCase const& testCase : kModeCases)
    {
      CAPTURE(static_cast<int>(testCase.mode));
      auto const temp = ao::test::TempFile{buildConstantModeStream(testCase.mode, 5), ".mp3"};
      auto decoderPtr = ao::test::requireValue(Mp3DecoderSession::open(temp.path, std::nullopt));
      auto const info = decoderPtr->streamInfo();
      decodeSilentStream(*decoderPtr, 5U * kLayerIiiPcmFrames);

      REQUIRE(decoderPtr->seek(std::chrono::milliseconds{0}));
      decoderPtr->flush();
      CHECK(decoderPtr->streamInfo() == info);
      decodeSilentStream(*decoderPtr, 5U * kLayerIiiPcmFrames);
    }
  }

  TEST_CASE("Mp3DecoderSession - accepts stereo and joint drift while keeping the published identity stable")
  {
    auto const driftStreams = std::vector<DriftStream>{
      {kStereoCase,
       {Mp3ChannelMode::Stereo,
        Mp3ChannelMode::Stereo,
        Mp3ChannelMode::Stereo,
        Mp3ChannelMode::JointStereo,
        Mp3ChannelMode::JointStereo,
        Mp3ChannelMode::JointStereo}},
      {kJointCase,
       {Mp3ChannelMode::JointStereo,
        Mp3ChannelMode::JointStereo,
        Mp3ChannelMode::JointStereo,
        Mp3ChannelMode::Stereo,
        Mp3ChannelMode::Stereo,
        Mp3ChannelMode::Stereo}},
      {kStereoCase,
       {Mp3ChannelMode::Stereo,
        Mp3ChannelMode::Stereo,
        Mp3ChannelMode::JointStereo,
        Mp3ChannelMode::JointStereo,
        Mp3ChannelMode::Stereo}},
    };

    for (std::size_t streamIndex = 0; streamIndex < driftStreams.size(); ++streamIndex)
    {
      CAPTURE(streamIndex, static_cast<int>(driftStreams[streamIndex].initialCase.mode));
      auto const& driftStream = driftStreams[streamIndex];
      auto const temp = ao::test::TempFile{buildModeStream(driftStream.frameModes), ".mp3"};
      auto decoderPtr = ao::test::requireValue(Mp3DecoderSession::open(temp.path, std::nullopt));
      auto const initialInfo = decoderPtr->streamInfo();
      checkModeIdentity(initialInfo, driftStream.initialCase, SampleEncoding::Signed16Le);
      decodeSilentStream(*decoderPtr, driftStream.frameModes.size() * kLayerIiiPcmFrames);
      CHECK(decoderPtr->streamInfo() == initialInfo);
    }
  }

  TEST_CASE("Mp3DecoderSession - same-count stereo and dual mixtures stay unknown and play every silent frame")
  {
    auto const encoding = GENERATE(SampleEncoding::Signed16Le,
                                   SampleEncoding::Signed24PackedLe,
                                   SampleEncoding::Signed24In32Le,
                                   SampleEncoding::Signed32Le,
                                   SampleEncoding::Float32Le);
    auto const streams = std::vector{
      buildModeRun(Mp3ChannelMode::Stereo, 3, Mp3ChannelMode::DualChannel, 3),
      buildModeRun(Mp3ChannelMode::DualChannel, 3, Mp3ChannelMode::Stereo, 3),
      buildModeRun(Mp3ChannelMode::Stereo, 2, Mp3ChannelMode::DualChannel, 2, Mp3ChannelMode::Stereo, 2),
      buildModeRun(Mp3ChannelMode::JointStereo, 3, Mp3ChannelMode::DualChannel, 3),
    };

    for (std::size_t streamIndex = 0; streamIndex < streams.size(); ++streamIndex)
    {
      CAPTURE(streamIndex, static_cast<int>(encoding));
      auto const temp = ao::test::TempFile{streams[streamIndex], ".mp3"};
      auto opened = openConcreteAndFactory(temp.path, encoding);
      auto const info = opened.concretePtr->streamInfo();
      checkWithheldIdentity(info, 2, kSampleRate44100, encoding);

      // Six Layer III frames are 6912 PCM frames, which floors to 156 ms.
      CHECK(info.duration == std::chrono::milliseconds{156});
      CHECK(opened.factoryPtr->streamInfo() == info);
      decodeSilentStream(*opened.concretePtr, 6912);
      decodeSilentStream(*opened.factoryPtr, 6912);
      CHECK(opened.concretePtr->streamInfo() == info);
      CHECK(opened.factoryPtr->streamInfo() == info);
    }
  }

  TEST_CASE("Mp3DecoderSession - seeks into and beyond a dual region keep the unknown layout")
  {
    auto const encoding = GENERATE(SampleEncoding::Signed16Le,
                                   SampleEncoding::Signed24PackedLe,
                                   SampleEncoding::Signed24In32Le,
                                   SampleEncoding::Signed32Le,
                                   SampleEncoding::Float32Le);

    // Frames 2-3 are dual (2304..4608). 80 ms is sample 3528, inside that
    // region. 120 ms is sample 5292, in the following stereo frame.
    auto const transient =
      buildModeRun(Mp3ChannelMode::Stereo, 2, Mp3ChannelMode::DualChannel, 2, Mp3ChannelMode::Stereo, 2);
    auto const finalDual = buildModeRun(Mp3ChannelMode::Stereo, 3, Mp3ChannelMode::DualChannel, 3);
    auto const dualPrefix = buildModeRun(Mp3ChannelMode::DualChannel, 3, Mp3ChannelMode::Stereo, 3);
    auto const streams = std::vector{transient, finalDual, dualPrefix};

    for (std::size_t streamIndex = 0; streamIndex < streams.size(); ++streamIndex)
    {
      CAPTURE(streamIndex, static_cast<int>(encoding));
      auto const temp = ao::test::TempFile{streams[streamIndex], ".mp3"};
      auto opened = openConcreteAndFactory(temp.path, encoding);
      auto& decoder = *opened.concretePtr;
      auto const info = decoder.streamInfo();
      checkWithheldIdentity(info, 2, kSampleRate44100, encoding);
      CHECK(info.duration == std::chrono::milliseconds{156});
      CHECK(opened.factoryPtr->streamInfo() == info);

      if (streamIndex == 0)
      {
        REQUIRE(decoder.seek(std::chrono::milliseconds{80}));
        decodeSilentRange(decoder, 3528, 6912, info);
        expectStableEndOfStream(decoder, info);

        REQUIRE(decoder.seek(std::chrono::milliseconds{120}));
        decodeSilentRange(decoder, 5292, 6912, info);
        expectStableEndOfStream(decoder, info);

        REQUIRE(opened.factoryPtr->seek(std::chrono::milliseconds{120}));
        auto const factoryBlockRes = opened.factoryPtr->readNextBlock();
        REQUIRE(factoryBlockRes);
        checkSilentBlock(*factoryBlockRes, 5292, info.outputFormat.channels, bytesPerSample(encoding));
        CHECK(opened.factoryPtr->streamInfo() == info);
      }
      else
      {
        // 100 ms is sample 4410: the first dual frame of a final-dual stream,
        // and the first stereo frame after a dual prefix. Neither promotes.
        REQUIRE(decoder.seek(std::chrono::milliseconds{100}));
        decodeSilentRange(decoder, 4410, 6912, info);
        expectStableEndOfStream(decoder, info);
      }

      REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
      CHECK(decoder.streamInfo() == info);
      decodeSilentStream(decoder, 6912);
      CHECK(decoder.streamInfo() == info);
    }
  }

  TEST_CASE("Mp3DecoderSession - channel or rate drift stays unknown and rejects the changed geometry before PCM")
  {
    auto const geometryStreams = std::vector<GeometryStream>{
      {.channels = 1,
       .optDuration = std::chrono::milliseconds{156},
       .frames = {{.mode = Mp3ChannelMode::Mono},
                  {.mode = Mp3ChannelMode::Mono},
                  {.mode = Mp3ChannelMode::Mono},
                  {.mode = Mp3ChannelMode::Stereo},
                  {.mode = Mp3ChannelMode::Stereo},
                  {.mode = Mp3ChannelMode::Stereo}}},
      {.channels = 2,
       .optDuration = std::chrono::milliseconds{156},
       .frames = {{.mode = Mp3ChannelMode::Stereo},
                  {.mode = Mp3ChannelMode::Stereo},
                  {.mode = Mp3ChannelMode::Stereo},
                  {.mode = Mp3ChannelMode::Mono},
                  {.mode = Mp3ChannelMode::Mono},
                  {.mode = Mp3ChannelMode::Mono}}},
      // A final-frame geometry change is refused before any contradictory PCM, even at the end.
      {.channels = 2,
       .optDuration = std::chrono::milliseconds{104},
       .frames = {{.mode = Mp3ChannelMode::Stereo},
                  {.mode = Mp3ChannelMode::Stereo},
                  {.mode = Mp3ChannelMode::Stereo},
                  {.mode = Mp3ChannelMode::Mono}}},
      {.channels = 2,
       .frames = {{.mode = Mp3ChannelMode::Stereo, .kind = MpegFrameKind::LayerIii44100},
                  {.mode = Mp3ChannelMode::Stereo, .kind = MpegFrameKind::LayerIii44100},
                  {.mode = Mp3ChannelMode::Stereo, .kind = MpegFrameKind::LayerIii44100},
                  {.mode = Mp3ChannelMode::Stereo, .kind = MpegFrameKind::LayerIii48000},
                  {.mode = Mp3ChannelMode::Stereo, .kind = MpegFrameKind::LayerIii48000},
                  {.mode = Mp3ChannelMode::Stereo, .kind = MpegFrameKind::LayerIii48000}}},
    };

    for (std::size_t streamIndex = 0; streamIndex < geometryStreams.size(); ++streamIndex)
    {
      CAPTURE(streamIndex, geometryStreams[streamIndex].channels, geometryStreams[streamIndex].sampleRate);
      auto const& geometryStream = geometryStreams[streamIndex];
      auto const temp = ao::test::TempFile{buildFrameStream(geometryStream.frames), ".mp3"};
      auto opened = openConcreteAndFactory(temp.path, std::nullopt);
      auto& decoder = *opened.concretePtr;
      auto const initialInfo = decoder.streamInfo();
      checkWithheldIdentity(
        initialInfo, geometryStream.channels, geometryStream.sampleRate, SampleEncoding::Signed16Le);
      CHECK(opened.factoryPtr->streamInfo() == initialInfo);

      if (geometryStream.optDuration)
      {
        CHECK(initialInfo.duration == *geometryStream.optDuration);
      }

      auto const deliveredFrames = consumeSilentPrefixUntilDrift(decoder, kGoodPrefixFrames);
      CHECK(deliveredFrames == 3456);
      CHECK(decoder.streamInfo() == initialInfo);
      checkPersistentNotSupported(decoder, initialInfo);

      REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
      CHECK(decoder.streamInfo() == initialInfo);
      auto const recoveredFrames = consumeSilentPrefixUntilDrift(decoder, kGoodPrefixFrames);
      CHECK(recoveredFrames == 3456);
      CHECK(decoder.streamInfo() == initialInfo);

      // 100 ms is sample 4410, past the 3456-frame intact prefix.
      checkSeekRefusesChangedGeometry(decoder, std::chrono::milliseconds{100}, initialInfo);
    }
  }

  TEST_CASE("Mp3DecoderSession - a seek beyond transient mono resumes the intact stereo PCM")
  {
    auto const frames = std::vector<MpegFrameSpec>{{.mode = Mp3ChannelMode::Stereo},
                                                   {.mode = Mp3ChannelMode::Stereo},
                                                   {.mode = Mp3ChannelMode::Stereo},
                                                   {.mode = Mp3ChannelMode::Mono},
                                                   {.mode = Mp3ChannelMode::Stereo},
                                                   {.mode = Mp3ChannelMode::Stereo}};
    auto const temp = ao::test::TempFile{buildFrameStream(frames), ".mp3"};
    auto opened = openConcreteAndFactory(temp.path, std::nullopt);
    auto& decoder = *opened.concretePtr;
    auto const initialInfo = decoder.streamInfo();
    checkWithheldIdentity(initialInfo, 2, kSampleRate44100, SampleEncoding::Signed16Le);
    CHECK(initialInfo.duration == std::chrono::milliseconds{156});
    CHECK(opened.factoryPtr->streamInfo() == initialInfo);

    auto const deliveredFrames = consumeSilentPrefixUntilDrift(decoder, kGoodPrefixFrames);
    CHECK(deliveredFrames == 3456);
    checkPersistentNotSupported(decoder, initialInfo);

    // 100 ms is sample 4410, inside the mono frame at 3456..4608.
    checkSeekRefusesChangedGeometry(decoder, std::chrono::milliseconds{100}, initialInfo);

    // 120 ms is sample 5292, in the stereo frame that follows the mono frame.
    REQUIRE(decoder.seek(std::chrono::milliseconds{120}));
    CHECK(decoder.streamInfo() == initialInfo);
    decodeSilentRange(decoder, 5292, 6912, initialInfo);
    expectStableEndOfStream(decoder, initialInfo);

    REQUIRE(opened.factoryPtr->seek(std::chrono::milliseconds{120}));
    auto const factoryBlockRes = opened.factoryPtr->readNextBlock();
    REQUIRE(factoryBlockRes);
    checkSilentBlock(*factoryBlockRes, 5292, 2, bytesPerSample(SampleEncoding::Signed16Le));
    CHECK(opened.factoryPtr->streamInfo() == initialInfo);

    REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
    auto const recoveredFrames = consumeSilentPrefixUntilDrift(decoder, kGoodPrefixFrames);
    CHECK(recoveredFrames == 3456);
    CHECK(decoder.streamInfo() == initialInfo);
  }

  TEST_CASE("Mp3DecoderSession - Layer I frames followed by Layer III keep stereo identity and exact PCM extent")
  {
    auto const encoding = GENERATE(SampleEncoding::Signed16Le,
                                   SampleEncoding::Signed24PackedLe,
                                   SampleEncoding::Signed24In32Le,
                                   SampleEncoding::Signed32Le,
                                   SampleEncoding::Float32Le);
    auto const temp = ao::test::TempFile{buildLayerTransitionStream(), ".mp3"};
    auto opened = openConcreteAndFactory(temp.path, encoding);
    auto const info = opened.concretePtr->streamInfo();
    checkModeIdentity(info, kStereoCase, encoding);

    // Five 384-frame Layer I frames plus five 1152-frame Layer III frames.
    CHECK(info.duration == std::chrono::milliseconds{174});
    CHECK(opened.factoryPtr->streamInfo() == info);
    decodeLayerTransition(*opened.concretePtr, info);
    decodeLayerTransition(*opened.factoryPtr, info);
  }

  TEST_CASE("Mp3DecoderSession - pass-through PCM bytes remain valid when seek reallocates the native buffer")
  {
    auto const temp = ao::test::TempFile{buildLayerTransitionStream(), ".mp3"};
    auto decoderPtr = ao::test::requireValue(Mp3DecoderSession::open(temp.path, std::nullopt));
    auto& decoder = *decoderPtr;
    auto const info = decoder.streamInfo();
    checkModeIdentity(info, kStereoCase, SampleEncoding::Signed16Le);
    auto const channels = info.outputFormat.channels;
    auto const sampleBytes = bytesPerSample(info.outputFormat.encoding);
    std::uint64_t totalFrames = 0;

    for (std::uint32_t frameIndex = 0; frameIndex < 5U; ++frameIndex)
    {
      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);
      REQUIRE(blockRes->frames == kLayerIPcmFrames);
      checkSilentBlock(*blockRes, totalFrames, channels, sampleBytes);
      totalFrames += blockRes->frames;
    }

    auto const layerIiiRes = decoder.readNextBlock();
    REQUIRE(layerIiiRes);
    REQUIRE(layerIiiRes->frames == kLayerIiiPcmFrames);
    CHECK(layerIiiRes->firstFrameIndex == 1920);
    checkSilentBlock(*layerIiiRes, 1920, channels, sampleBytes);

    // Signed16 pass-through borrows the session buffer. Seeking back to the
    // shorter Layer I frame can reallocate the native buffer before the next read.
    auto const heldBlock = *layerIiiRes;
    auto const heldBytes = std::vector<std::byte>{heldBlock.bytes.begin(), heldBlock.bytes.end()};
    REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
    CHECK(decoder.streamInfo() == info);
    REQUIRE(heldBlock.bytes.data() != nullptr);
    REQUIRE(heldBlock.bytes.size() == heldBytes.size());
    CHECK(std::ranges::equal(heldBlock.bytes, heldBytes));
    CHECK(heldBlock.frames == kLayerIiiPcmFrames);
    CHECK(heldBlock.firstFrameIndex == 1920);

    auto const restartedRes = decoder.readNextBlock();
    REQUIRE(restartedRes);
    REQUIRE(restartedRes->frames == kLayerIPcmFrames);
    checkSilentBlock(*restartedRes, 0, channels, sampleBytes);
    CHECK(decoder.streamInfo() == info);
  }

  TEST_CASE("Mp3DecoderSession - a garbage gap or truncated tail withholds identity and keeps intact playback")
  {
    auto const leading = buildConstantModeStream(Mp3ChannelMode::Stereo, 3);
    auto const trailing = buildConstantModeStream(Mp3ChannelMode::Stereo, 3);
    auto garbageGap = leading;
    garbageGap.insert(garbageGap.end(), 32, 0x11);
    garbageGap.insert(garbageGap.end(), trailing.begin(), trailing.end());

    auto truncatedTail = buildConstantModeStream(Mp3ChannelMode::Stereo, 5);
    auto const partialFrame = buildMpegFrameBytes(Mp3ChannelMode::Stereo);
    truncatedTail.insert(truncatedTail.end(), partialFrame.begin(), partialFrame.begin() + 20);

    auto const nonTagTail = [&]
    {
      auto data = buildConstantModeStream(Mp3ChannelMode::Stereo, 5);
      data.resize(data.size() + kId3v1ByteCount, 0U);

      return data;
    }();

    auto const streams = std::vector<std::pair<std::vector<std::uint8_t>, std::uint64_t>>{
      {garbageGap, 6912},
      {truncatedTail, 5760},
      {nonTagTail, 5760},
    };

    for (std::size_t streamIndex = 0; streamIndex < streams.size(); ++streamIndex)
    {
      CAPTURE(streamIndex);
      auto const& [data, pcmFrames] = streams[streamIndex];
      auto const temp = ao::test::TempFile{data, ".mp3"};
      auto opened = openConcreteAndFactory(temp.path, std::nullopt);
      auto const info = opened.concretePtr->streamInfo();
      checkWithheldIdentity(info, 2, kSampleRate44100, SampleEncoding::Signed16Le);
      CHECK(opened.factoryPtr->streamInfo() == info);
      decodeSilentStream(*opened.concretePtr, pcmFrames);
      CHECK(opened.concretePtr->streamInfo() == info);
    }
  }

  TEST_CASE("Mp3DecoderSession - an ID3v1 tail on uniform stereo stays identified")
  {
    auto data = buildConstantModeStream(Mp3ChannelMode::Stereo, 5);
    appendId3v1Tag(data);
    auto const temp = ao::test::TempFile{data, ".mp3"};
    auto opened = openConcreteAndFactory(temp.path, std::nullopt);
    auto const info = opened.concretePtr->streamInfo();
    checkModeIdentity(info, kStereoCase, SampleEncoding::Signed16Le);
    CHECK(info.duration == std::chrono::milliseconds{130});
    CHECK(opened.factoryPtr->streamInfo() == info);
    decodeSilentStream(*opened.concretePtr, 5U * kLayerIiiPcmFrames);
    decodeSilentStream(*opened.factoryPtr, 5U * kLayerIiiPcmFrames);
    CHECK(opened.concretePtr->streamInfo() == info);
  }

  TEST_CASE("Mp3DecoderSession - complete indexing catches a late dual past sixteen thousand frames")
  {
    auto const uniform = buildConstantModeStream(Mp3ChannelMode::Stereo, kIndexProbeFrameCount);
    auto lateDualModes = std::vector<Mp3ChannelMode>(kIndexProbeFrameCount, Mp3ChannelMode::Stereo);
    lateDualModes.back() = Mp3ChannelMode::DualChannel;
    auto const lateDual = buildModeStream(lateDualModes);
    auto const uniformTemp = ao::test::TempFile{uniform, ".mp3"};
    auto const lateDualTemp = ao::test::TempFile{lateDual, ".mp3"};
    auto uniformOpened = openConcreteAndFactory(uniformTemp.path, std::nullopt);
    auto lateDualOpened = openConcreteAndFactory(lateDualTemp.path, std::nullopt);
    auto const uniformInfo = uniformOpened.concretePtr->streamInfo();
    auto const lateDualInfo = lateDualOpened.concretePtr->streamInfo();
    checkModeIdentity(uniformInfo, kStereoCase, SampleEncoding::Signed16Le);
    checkWithheldIdentity(lateDualInfo, 2, kSampleRate44100, SampleEncoding::Signed16Le);

    // 16385 Layer III frames are 18875520 PCM frames, which floors to 428016 ms.
    CHECK(uniformInfo.duration == std::chrono::milliseconds{428016});
    CHECK(lateDualInfo.duration == std::chrono::milliseconds{428016});
    CHECK(uniformOpened.factoryPtr->streamInfo() == uniformInfo);
    CHECK(lateDualOpened.factoryPtr->streamInfo() == lateDualInfo);

    auto const uniformBlockRes = uniformOpened.concretePtr->readNextBlock();
    REQUIRE(uniformBlockRes);
    checkSilentBlock(*uniformBlockRes, 0, 2, bytesPerSample(SampleEncoding::Signed16Le));
    CHECK(uniformOpened.concretePtr->streamInfo() == uniformInfo);
    decodeSilentStream(*lateDualOpened.concretePtr, kIndexProbeFrameCount * kLayerIiiPcmFrames);
    CHECK(lateDualOpened.concretePtr->streamInfo() == lateDualInfo);
  }
} // namespace ao::audio::test
