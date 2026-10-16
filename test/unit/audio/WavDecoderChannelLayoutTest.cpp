// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/WavDecoderSession.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/media/wav/TestWav.h"
#include <ao/Error.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    // Independent WAVE role oracle, not a production layout/permutation table.
    enum class WavRole : std::uint32_t
    {
      FrontLeft = 0x1,
      FrontRight = 0x2,
      FrontCenter = 0x4,
      LowFrequency = 0x8,
      BackLeft = 0x10,
      BackRight = 0x20,
      FrontLeftOfCenter = 0x40,
      FrontRightOfCenter = 0x80,
      BackCenter = 0x100,
      SideLeft = 0x200,
      SideRight = 0x400,
      TopCenter = 0x800,
      TopFrontLeft = 0x1000,
      TopFrontCenter = 0x2000,
      TopFrontRight = 0x4000,
      TopBackLeft = 0x8000,
      TopBackCenter = 0x10000,
      TopBackRight = 0x20000,
    };

    struct LayoutCase final
    {
      std::uint16_t channels;
      std::uint32_t mask;
      std::vector<WavRole> roles;
      // Two complete interleaved S16LE frames in the independently named order.
      std::vector<std::uint8_t> pcm;
    };

    std::span<std::byte const> pcmBytes(std::vector<std::uint8_t> const& bytes) noexcept
    {
      return utility::bytes::view(std::span<std::uint8_t const>{bytes});
    }

    void checkLayout(DecodedStreamInfo const& info,
                     LayoutCase const& expected,
                     ChannelLayoutOrigin origin,
                     std::optional<std::uint32_t> optDeclaration)
    {
      CHECK(info.sourceFormat.channels == expected.channels);
      CHECK(info.outputFormat.channels == expected.channels);
      CHECK(info.sourceFormat.sampleRate == 1000);
      CHECK(info.outputFormat.sampleRate == 1000);
      CHECK(info.sourceFormat.speakerMask == expected.mask);
      CHECK(info.outputFormat.speakerMask == expected.mask);
      CHECK(info.sourceChannelLayout.origin == origin);
      CHECK(info.sourceChannelLayout.optDeclaredMask == optDeclaration);
      REQUIRE(expected.roles.size() == expected.channels);

      auto remainingMask = info.sourceFormat.speakerMask;

      for (auto const role : expected.roles)
      {
        REQUIRE(remainingMask != 0);
        auto const nextPosition = remainingMask & (~remainingMask + 1U);
        CHECK(nextPosition == static_cast<std::uint32_t>(role));
        remainingMask &= ~nextPosition;
      }

      CHECK(remainingMask == 0);
    }

    void checkTwoFramePcm(WavDecoderSession& decoder, std::vector<std::uint8_t> const& expected)
    {
      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);
      CHECK(blockRes->firstFrameIndex == 0);
      CHECK(blockRes->frames == 2);
      CHECK(blockRes->endOfStream);
      CHECK(std::ranges::equal(blockRes->bytes, pcmBytes(expected)));
      CHECK(blockRes->bytes.size() == 2U * frameBytes(decoder.streamInfo().outputFormat));

      for (std::size_t index = 0; index < 2; ++index)
      {
        auto const eofRes = decoder.readNextBlock();
        REQUIRE(eofRes);
        CHECK(eofRes->endOfStream);
        CHECK(eofRes->frames == 0);
        CHECK(eofRes->bytes.empty());
      }
    }
  } // namespace

  TEST_CASE("WavDecoderSession - assigns every absent and explicit-zero WAV compatibility layout",
            "[audio][integration][wav][decoder]")
  {
    using enum WavRole;
    auto const cases = std::vector<LayoutCase>{
      {1, 0x4, {FrontCenter}, {0x01, 0x10, 0x81, 0xE0}},
      {2, 0x3, {FrontLeft, FrontRight}, {0x01, 0x10, 0x02, 0x20, 0x81, 0xE0, 0x82, 0xD0}},
      {3,
       0x7,
       {FrontLeft, FrontRight, FrontCenter},
       {0x01, 0x10, 0x02, 0x20, 0x03, 0x30, 0x81, 0xE0, 0x82, 0xD0, 0x83, 0xC0}},
      {4,
       0x33,
       {FrontLeft, FrontRight, BackLeft, BackRight},
       {0x01, 0x10, 0x02, 0x20, 0x03, 0x30, 0x04, 0x40, 0x81, 0xE0, 0x82, 0xD0, 0x83, 0xC0, 0x84, 0xB0}},
      {5, 0x37, {FrontLeft, FrontRight, FrontCenter, BackLeft, BackRight}, {0x01, 0x10, 0x02, 0x20, 0x03, 0x30, 0x04,
                                                                            0x40, 0x05, 0x50, 0x81, 0xE0, 0x82, 0xD0,
                                                                            0x83, 0xC0, 0x84, 0xB0, 0x85, 0xA0}},
      {6,
       0x3F,
       {FrontLeft, FrontRight, FrontCenter, LowFrequency, BackLeft, BackRight},
       {0x01, 0x10, 0x02, 0x20, 0x03, 0x30, 0x04, 0x40, 0x05, 0x50, 0x06, 0x60,
        0x81, 0xE0, 0x82, 0xD0, 0x83, 0xC0, 0x84, 0xB0, 0x85, 0xA0, 0x86, 0x90}},
      {7,
       0x70F,
       {FrontLeft, FrontRight, FrontCenter, LowFrequency, BackCenter, SideLeft, SideRight},
       {0x01, 0x10, 0x02, 0x20, 0x03, 0x30, 0x04, 0x40, 0x05, 0x50, 0x06, 0x60, 0x07, 0x70,
        0x81, 0xE0, 0x82, 0xD0, 0x83, 0xC0, 0x84, 0xB0, 0x85, 0xA0, 0x86, 0x90, 0x87, 0x80}},
      {8,
       0x63F,
       {FrontLeft, FrontRight, FrontCenter, LowFrequency, BackLeft, BackRight, SideLeft, SideRight},
       {0x01, 0x10, 0x02, 0x20, 0x03, 0x30, 0x04, 0x40, 0x05, 0x50, 0x06, 0x60, 0x07, 0x70, 0x08, 0x01,
        0x81, 0xE0, 0x82, 0xD0, 0x83, 0xC0, 0x84, 0xB0, 0x85, 0xA0, 0x86, 0x90, 0x87, 0x80, 0x88, 0xF0}},
    };

    for (auto const& expected : cases)
    {
      for (bool const hasDeclaration : {false, true})
      {
        CAPTURE(expected.channels, hasDeclaration);
        auto const data = ao::test::wav::makeWav({
          .sampleFormat =
            hasDeclaration ? ao::test::wav::SampleFormat::ExtensiblePcm : ao::test::wav::SampleFormat::Pcm,
          .channels = expected.channels,
          .channelMask = 0,
          .audioData = expected.pcm,
        });
        auto const temp = ao::test::TempFile{data, ".wav"};
        auto decoderPtr = ao::test::requireValue(WavDecoderSession::open(temp.path, std::nullopt));
        auto const info = decoderPtr->streamInfo();
        auto const origin = !hasDeclaration && expected.channels <= 2 ? ChannelLayoutOrigin::FormatDefault
                                                                      : ChannelLayoutOrigin::Inferred;
        auto const optDeclaration = hasDeclaration ? std::optional<std::uint32_t>{0} : std::nullopt;
        checkLayout(info, expected, origin, optDeclaration);
        CHECK(info.sourceFormat.precisionBits == 16);
        CHECK(info.outputFormat.encoding == SampleEncoding::Signed16Le);
        CHECK(signalFormat(info.outputFormat) == info.sourceFormat);
        CHECK(info.duration == std::chrono::milliseconds{2});
        checkTwoFramePcm(*decoderPtr, expected.pcm);
      }
    }
  }

  TEST_CASE("WavDecoderSession - preserves complete alternate declarations without count-based relabeling",
            "[audio][integration][wav][decoder]")
  {
    using enum WavRole;
    auto const cases = std::vector<LayoutCase>{
      {1, 0x1, {FrontLeft}, {0x34, 0x12, 0xCB, 0xED}},
      {1, 0x2, {FrontRight}, {0x34, 0x12, 0xCB, 0xED}},
      {1, 0x4, {FrontCenter}, {0x34, 0x12, 0xCB, 0xED}},
      {1, 0x20000, {TopBackRight}, {0x34, 0x12, 0xCB, 0xED}},
      {2, 0x3, {FrontLeft, FrontRight}, {0x11, 0x22, 0x33, 0x44, 0xAA, 0xBB, 0xCC, 0xDD}},
      {2, 0xC, {FrontCenter, LowFrequency}, {0x11, 0x22, 0x33, 0x44, 0xAA, 0xBB, 0xCC, 0xDD}},
      {2, 0x30, {BackLeft, BackRight}, {0x11, 0x22, 0x33, 0x44, 0xAA, 0xBB, 0xCC, 0xDD}},
      {2, 0xC0, {FrontLeftOfCenter, FrontRightOfCenter}, {0x11, 0x22, 0x33, 0x44, 0xAA, 0xBB, 0xCC, 0xDD}},
      {2, 0x30000, {TopBackCenter, TopBackRight}, {0x11, 0x22, 0x33, 0x44, 0xAA, 0xBB, 0xCC, 0xDD}},
      {4,
       0x33,
       {FrontLeft, FrontRight, BackLeft, BackRight},
       {0x01, 0x10, 0x02, 0x20, 0x03, 0x30, 0x04, 0x40, 0x81, 0xE0, 0x82, 0xD0, 0x83, 0xC0, 0x84, 0xB0}},
      {4,
       0x107,
       {FrontLeft, FrontRight, FrontCenter, BackCenter},
       {0x01, 0x10, 0x02, 0x20, 0x03, 0x30, 0x04, 0x40, 0x81, 0xE0, 0x82, 0xD0, 0x83, 0xC0, 0x84, 0xB0}},
      {9,
       0x1FF,
       {FrontLeft,
        FrontRight,
        FrontCenter,
        LowFrequency,
        BackLeft,
        BackRight,
        FrontLeftOfCenter,
        FrontRightOfCenter,
        BackCenter},
       {0x01, 0x10, 0x02, 0x20, 0x03, 0x30, 0x04, 0x40, 0x05, 0x50, 0x06, 0x60, 0x07, 0x70, 0x08, 0x01, 0x09, 0x02,
        0x81, 0xE0, 0x82, 0xD0, 0x83, 0xC0, 0x84, 0xB0, 0x85, 0xA0, 0x86, 0x90, 0x87, 0x80, 0x88, 0xF0, 0x89, 0xE0}},
      {18,
       0x3FFFF,
       {FrontLeft,
        FrontRight,
        FrontCenter,
        LowFrequency,
        BackLeft,
        BackRight,
        FrontLeftOfCenter,
        FrontRightOfCenter,
        BackCenter,
        SideLeft,
        SideRight,
        TopCenter,
        TopFrontLeft,
        TopFrontCenter,
        TopFrontRight,
        TopBackLeft,
        TopBackCenter,
        TopBackRight},
       {0x01, 0x10, 0x02, 0x20, 0x03, 0x30, 0x04, 0x40, 0x05, 0x50, 0x06, 0x60, 0x07, 0x70, 0x08, 0x01, 0x09, 0x02,
        0x0A, 0x03, 0x0B, 0x04, 0x0C, 0x05, 0x0D, 0x06, 0x0E, 0x07, 0x0F, 0x08, 0x10, 0x09, 0x11, 0x0A, 0x12, 0x0B,
        0x81, 0xE0, 0x82, 0xD0, 0x83, 0xC0, 0x84, 0xB0, 0x85, 0xA0, 0x86, 0x90, 0x87, 0x80, 0x88, 0xF0, 0x89, 0xE0,
        0x8A, 0xD0, 0x8B, 0xC0, 0x8C, 0xB0, 0x8D, 0xA0, 0x8E, 0x90, 0x8F, 0x80, 0x90, 0xF0, 0x91, 0xE0, 0x92, 0xD0}},
    };

    for (auto const& expected : cases)
    {
      CAPTURE(expected.channels, expected.mask);
      auto const data = ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                                .channels = expected.channels,
                                                .channelMask = expected.mask,
                                                .audioData = expected.pcm});
      auto const temp = ao::test::TempFile{data, ".wav"};
      auto decoderPtr = ao::test::requireValue(WavDecoderSession::open(temp.path, std::nullopt));
      checkLayout(decoderPtr->streamInfo(), expected, ChannelLayoutOrigin::Declared, expected.mask);
      checkTwoFramePcm(*decoderPtr, expected.pcm);
    }
  }

  TEST_CASE("WavDecoderSession - ignores excess highest positions while retaining the original declaration",
            "[audio][integration][wav][decoder]")
  {
    using enum WavRole;
    struct ExcessCase final
    {
      std::uint32_t declaration;
      LayoutCase effective;
    };
    auto const cases = std::vector<ExcessCase>{
      {0x3, {1, 0x1, {FrontLeft}, {0x34, 0x12, 0xCB, 0xED}}},
      {0x20004, {1, 0x4, {FrontCenter}, {0x34, 0x12, 0xCB, 0xED}}},
      {0x3F, {2, 0x3, {FrontLeft, FrontRight}, {0x11, 0x22, 0x33, 0x44, 0xAA, 0xBB, 0xCC, 0xDD}}},
      {0x3FFFF,
       {4,
        0xF,
        {FrontLeft, FrontRight, FrontCenter, LowFrequency},
        {0x01, 0x10, 0x02, 0x20, 0x03, 0x30, 0x04, 0x40, 0x81, 0xE0, 0x82, 0xD0, 0x83, 0xC0, 0x84, 0xB0}}},
      {0x40040033,
       {4,
        0x33,
        {FrontLeft, FrontRight, BackLeft, BackRight},
        {0x01, 0x10, 0x02, 0x20, 0x03, 0x30, 0x04, 0x40, 0x81, 0xE0, 0x82, 0xD0, 0x83, 0xC0, 0x84, 0xB0}}},
    };

    for (auto const& testCase : cases)
    {
      CAPTURE(testCase.declaration, testCase.effective.channels);
      auto const data = ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                                .channels = testCase.effective.channels,
                                                .channelMask = testCase.declaration,
                                                .audioData = testCase.effective.pcm});
      auto const temp = ao::test::TempFile{data, ".wav"};
      auto decoderPtr = ao::test::requireValue(WavDecoderSession::open(temp.path, std::nullopt));
      checkLayout(decoderPtr->streamInfo(), testCase.effective, ChannelLayoutOrigin::Declared, testCase.declaration);
      checkTwoFramePcm(*decoderPtr, testCase.effective.pcm);
    }
  }

  TEST_CASE("WavDecoderSession - returns contextual source errors for partial reserved and wildcard declarations",
            "[audio][integration][wav][decoder]")
  {
    struct RefusalCase final
    {
      std::uint16_t channels;
      std::uint32_t mask;
      std::string_view reason;
    };
    auto const cases = std::to_array<RefusalCase>({
      {2, 0x1, "partially"},
      {4, 0x7, "partially"},
      {19, 0x3FFFF, "partially"},
      {1, 0x40000, "reserved"},
      {2, 0x40001, "reserved"},
      {3, 0x40040001, "reserved"},
      {1, 0x80000000, "wildcard"},
      {2, 0x80000003, "wildcard"},
      {4, 0xFFFFFFFF, "wildcard"},
    });

    for (auto const& testCase : cases)
    {
      CAPTURE(testCase.channels, testCase.mask);
      auto const data = ao::test::wav::makeWav(
        {.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
         .channels = testCase.channels,
         .channelMask = testCase.mask,
         .audioData = std::vector<std::uint8_t>(static_cast<std::size_t>(testCase.channels) * 2U)});
      auto const temp = ao::test::TempFile{data, ".wav"};
      auto const openRes = WavDecoderSession::open(temp.path, std::nullopt);
      REQUIRE_FALSE(openRes);
      CHECK(openRes.error().code == Error::Code::NotSupported);
      CHECK(openRes.error().message.contains("WAV channel layout"));
      CHECK(openRes.error().message.contains(std::format("0x{:08X}", testCase.mask)));
      CHECK(openRes.error().message.contains(std::format("{} channels", testCase.channels)));
      CHECK(openRes.error().message.contains(testCase.reason));
    }
  }

  TEST_CASE("WavDecoderSession - invents no absent or zero-mask fallback outside the WAV table",
            "[audio][integration][wav][decoder]")
  {
    for (auto const channels : std::to_array<std::uint16_t>({9, 18, 32, 33}))
    {
      for (bool const hasDeclaration : {false, true})
      {
        CAPTURE(channels, hasDeclaration);
        auto const data = ao::test::wav::makeWav({
          .sampleFormat =
            hasDeclaration ? ao::test::wav::SampleFormat::ExtensiblePcm : ao::test::wav::SampleFormat::Pcm,
          .channels = channels,
          .audioData = std::vector<std::uint8_t>(static_cast<std::size_t>(channels) * 2U),
        });
        auto const temp = ao::test::TempFile{data, ".wav"};
        auto const openRes = WavDecoderSession::open(temp.path, std::nullopt);
        REQUIRE_FALSE(openRes);
        CHECK(openRes.error().code == Error::Code::NotSupported);
        CHECK(openRes.error().message.contains("no supported fallback"));
        CHECK(openRes.error().message.contains(std::format("{} channels", channels)));
        CHECK(openRes.error().message.contains(hasDeclaration ? "explicit mask 0x00000000" : "no mask declaration"));
      }
    }
  }

  TEST_CASE("WavDecoderSession - keeps source identity through requested lossless output adaptation",
            "[audio][integration][wav][decoder]")
  {
    using enum WavRole;
    auto const source =
      LayoutCase{4,
                 0x107,
                 {FrontLeft, FrontRight, FrontCenter, BackCenter},
                 {0x00, 0x20, 0x00, 0x40, 0x00, 0xC0, 0x00, 0xE0, 0x00, 0x10, 0x00, 0xF0, 0x00, 0x60, 0x00, 0xA0}};
    struct AdaptationCase final
    {
      SampleEncoding encoding;
      std::vector<std::uint8_t> pcm;
    };
    auto const cases = std::vector<AdaptationCase>{
      {SampleEncoding::Signed32Le,
       {0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0xE0,
        0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xF0, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0xA0}},
      // +.25, +.5, -.5, -.25; +.125, -.125, +.75, -.75 in IEEE binary32 LE.
      {SampleEncoding::Float32Le,
       {0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x80, 0xBE,
        0x00, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x00, 0xBE, 0x00, 0x00, 0x40, 0x3F, 0x00, 0x00, 0x40, 0xBF}},
    };

    for (auto const& expected : cases)
    {
      CAPTURE(expected.encoding);
      auto const data = ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                                .channels = source.channels,
                                                .channelMask = source.mask,
                                                .audioData = source.pcm});
      auto const temp = ao::test::TempFile{data, ".wav"};
      auto decoderPtr = ao::test::requireValue(WavDecoderSession::open(temp.path, expected.encoding));
      auto const info = decoderPtr->streamInfo();
      checkLayout(info, source, ChannelLayoutOrigin::Declared, 0x107);
      CHECK(info.sourceFormat.precisionBits == 16);
      CHECK(info.sourceFormat.sampleKind == SampleKind::Integer);
      CHECK(info.outputFormat.encoding == expected.encoding);
      checkTwoFramePcm(*decoderPtr, expected.pcm);
    }
  }

  TEST_CASE("WavDecoderSession - preserves declared float slot identities and all native float bytes",
            "[audio][integration][wav][decoder]")
  {
    using enum WavRole;
    auto const layout = LayoutCase{2, 0xC, {FrontCenter, LowFrequency}, {}};
    // FC +.25, LFE -.5; FC +.75, LFE -.125, already in WAVE bit order.
    auto const pcm = std::vector<std::uint8_t>{
      0x00,
      0x00,
      0x80,
      0x3E,
      0x00,
      0x00,
      0x00,
      0xBF,
      0x00,
      0x00,
      0x40,
      0x3F,
      0x00,
      0x00,
      0x00,
      0xBE,
    };
    auto const data = ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::ExtensibleFloat,
                                              .channels = 2,
                                              .bitsPerSample = 32,
                                              .channelMask = 0xC,
                                              .audioData = pcm});
    auto const temp = ao::test::TempFile{data, ".wav"};
    auto decoderPtr = ao::test::requireValue(WavDecoderSession::open(temp.path, std::nullopt));
    auto const info = decoderPtr->streamInfo();
    checkLayout(info, layout, ChannelLayoutOrigin::Declared, 0xC);
    CHECK(info.sourceFormat.sampleKind == SampleKind::FloatingPoint);
    CHECK(info.sourceFormat.precisionBits == 32);
    CHECK(info.outputFormat.encoding == SampleEncoding::Float32Le);
    CHECK(signalFormat(info.outputFormat) == info.sourceFormat);
    checkTwoFramePcm(*decoderPtr, pcm);

    auto const integerRes = WavDecoderSession::open(temp.path, SampleEncoding::Signed32Le);
    REQUIRE_FALSE(integerRes);
    CHECK(integerRes.error().code == Error::Code::NotSupported);
  }

  TEST_CASE("WavDecoderSession - preserves layout stride and complete PCM across block boundaries seek and EOF",
            "[audio][integration][wav][decoder]")
  {
    using enum WavRole;
    auto const layout = LayoutCase{4, 0x107, {FrontLeft, FrontRight, FrontCenter, BackCenter}, {}};
    auto const repeatedFrame = std::to_array<std::uint8_t>({0x01, 0x10, 0x02, 0x20, 0x03, 0x30, 0x04, 0x40});
    auto const lastFrame = std::to_array<std::uint8_t>({0x81, 0xE0, 0x82, 0xD0, 0x83, 0xC0, 0x84, 0xB0});
    auto pcm = std::vector<std::uint8_t>{};

    for (std::size_t frameIndex = 0; frameIndex < 4096; ++frameIndex)
    {
      pcm.insert(pcm.end(), repeatedFrame.begin(), repeatedFrame.end());
    }

    pcm.insert(pcm.end(), lastFrame.begin(), lastFrame.end());
    auto const data = ao::test::wav::makeWav({.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                              .channels = 4,
                                              .channelMask = 0x107,
                                              .audioData = pcm});
    auto const temp = ao::test::TempFile{data, ".wav"};
    auto decoderPtr = ao::test::requireValue(WavDecoderSession::open(temp.path, std::nullopt));
    auto const info = decoderPtr->streamInfo();
    checkLayout(info, layout, ChannelLayoutOrigin::Declared, 0x107);
    CHECK(info.duration == std::chrono::milliseconds{4097});

    auto const firstRes = decoderPtr->readNextBlock();
    REQUIRE(firstRes);
    CHECK(firstRes->firstFrameIndex == 0);
    CHECK(firstRes->frames == 4096);
    CHECK_FALSE(firstRes->endOfStream);
    CHECK(std::ranges::equal(firstRes->bytes, pcmBytes(pcm).first(32768)));
    auto const finalRes = decoderPtr->readNextBlock();
    REQUIRE(finalRes);
    CHECK(finalRes->firstFrameIndex == 4096);
    CHECK(finalRes->frames == 1);
    CHECK(finalRes->endOfStream);
    CHECK(std::ranges::equal(finalRes->bytes, pcmBytes(pcm).last(8)));

    REQUIRE(decoderPtr->seek(std::chrono::milliseconds{1}));
    decoderPtr->flush();
    auto const seekRes = decoderPtr->readNextBlock();
    REQUIRE(seekRes);
    CHECK(seekRes->firstFrameIndex == 1);
    CHECK(seekRes->frames == 4096);
    CHECK(seekRes->endOfStream);
    CHECK(std::ranges::equal(seekRes->bytes, pcmBytes(pcm).subspan(8)));
    CHECK(decoderPtr->streamInfo() == info);

    REQUIRE(decoderPtr->seek(std::chrono::milliseconds{4096}));
    auto const lastRes = decoderPtr->readNextBlock();
    REQUIRE(lastRes);
    CHECK(lastRes->firstFrameIndex == 4096);
    CHECK(lastRes->frames == 1);
    CHECK(lastRes->endOfStream);
    CHECK(std::ranges::equal(lastRes->bytes, pcmBytes(pcm).last(8)));
    REQUIRE(decoderPtr->seek(std::chrono::milliseconds{4097}));

    for (std::size_t index = 0; index < 2; ++index)
    {
      auto const eofRes = decoderPtr->readNextBlock();
      REQUIRE(eofRes);
      CHECK(eofRes->endOfStream);
      CHECK(eofRes->frames == 0);
      CHECK(eofRes->bytes.empty());
    }

    auto const pastEndRes = decoderPtr->seek(std::chrono::milliseconds{4098});
    REQUIRE_FALSE(pastEndRes);
    CHECK(pastEndRes.error().code == Error::Code::SeekFailed);
    CHECK(decoderPtr->streamInfo() == info);
  }

  TEST_CASE("WavDecoderSession - layout interpretation does not admit zero geometry or invalid precision",
            "[audio][integration][wav][decoder]")
  {
    auto const spec = ao::test::wav::Spec{.sampleFormat = ao::test::wav::SampleFormat::ExtensiblePcm,
                                          .channels = 2,
                                          .bitsPerSample = 24,
                                          .validBitsPerSample = 24,
                                          .channelMask = 0x3,
                                          .audioData = {0, 0, 0, 0, 0, 0}};
    struct InvalidFormatCase final
    {
      std::size_t offset;
      std::uint16_t value;
      Error::Code code;
      std::string_view reason;
    };
    auto const cases = std::to_array<InvalidFormatCase>({
      {2, 0, Error::Code::CorruptData, "zero audio properties"},
      {4, 0, Error::Code::CorruptData, "zero audio properties"},
      {8, 0, Error::Code::CorruptData, "zero audio properties"},
      {12, 0, Error::Code::CorruptData, "zero audio properties"},
      {14, 0, Error::Code::CorruptData, "zero audio properties"},
      {18, 25, Error::Code::CorruptData, "invalid valid-bit depth"},
      {18, 20, Error::Code::NotSupported, "valid-bit/container mismatch"},
      {14, 12, Error::Code::CorruptData, "invalid valid-bit depth"},
    });

    for (auto const& testCase : cases)
    {
      CAPTURE(testCase.offset, testCase.value);
      auto format = ao::test::wav::makeFmtChunk(spec);
      format[testCase.offset] = static_cast<std::uint8_t>(testCase.value & 0xFFU);
      format[testCase.offset + 1] = static_cast<std::uint8_t>(testCase.value >> 8U);
      auto const data = ao::test::wav::makeWav(spec, format);
      auto const temp = ao::test::TempFile{data, ".wav"};
      auto const openRes = WavDecoderSession::open(temp.path, std::nullopt);
      REQUIRE_FALSE(openRes);
      CHECK(openRes.error().code == testCase.code);
      CHECK(openRes.error().message.contains(testCase.reason));
    }

    auto format = ao::test::wav::makeFmtChunk(spec);
    format[18] = 0; // Existing extensible zero-valid-bits normalization is unchanged.
    format[19] = 0;
    auto const data = ao::test::wav::makeWav(spec, format);
    auto const temp = ao::test::TempFile{data, ".wav"};
    auto decoderPtr = ao::test::requireValue(WavDecoderSession::open(temp.path, std::nullopt));
    CHECK(decoderPtr->streamInfo().sourceFormat.precisionBits == 24);
    CHECK(decoderPtr->streamInfo().sourceFormat.speakerMask == 0x3);
    CHECK(decoderPtr->streamInfo().outputFormat.speakerMask == 0x3);
    auto const losingRes = WavDecoderSession::open(temp.path, SampleEncoding::Signed16Le);
    REQUIRE_FALSE(losingRes);
    CHECK(losingRes.error().code == Error::Code::NotSupported);
    CHECK(losingRes.error().message.contains("loses source precision"));
  }
} // namespace ao::audio::test
