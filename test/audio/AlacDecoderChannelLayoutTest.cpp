// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AlacFrameTestSupport.h"
#include "lib/audio/AlacDecoderSession.h"
#include "lib/audio/DecoderFactory.h"
#include "test/TestFixtureSupport.h"
#include <ao/AudioCodec.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
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
#include <optional>
#include <span>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    // Independent WAVE role oracle. These are identities, not ALAC arrival slots.
    enum class Speaker : std::uint16_t
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
    };

    struct LayoutExpectation final
    {
      std::uint32_t mask;
      std::vector<Speaker> roles;
      std::vector<std::uint8_t> frame;
    };

    void checkAlacInfo(DecodedStreamInfo const& info,
                       AlacFrameTestOptions const& options,
                       LayoutExpectation const& expected,
                       SampleEncoding encoding)
    {
      CHECK(info.codec == AudioCodec::Alac);
      CHECK_FALSE(info.isLossy);
      CHECK(info.sourceFormat.channels == options.channels);
      CHECK(info.outputFormat.channels == options.channels);
      CHECK(info.sourceFormat.sampleRate == 48000);
      CHECK(info.outputFormat.sampleRate == 48000);
      CHECK(info.sourceFormat.precisionBits == options.precisionBits);
      CHECK(info.sourceFormat.sampleKind == SampleKind::Integer);
      CHECK(info.outputFormat.encoding == encoding);
      CHECK(info.sourceFormat.speakerMask == expected.mask);
      CHECK(info.outputFormat.speakerMask == expected.mask);
      CHECK(info.sourceChannelLayout.origin ==
            (expected.mask == 0 ? ChannelLayoutOrigin::Unknown : ChannelLayoutOrigin::Identified));

      if (expected.mask == 0)
      {
        CHECK(expected.roles.empty());
        return;
      }

      REQUIRE(expected.roles.size() == options.channels);
      auto remainingMask = info.sourceFormat.speakerMask;

      for (auto const role : expected.roles)
      {
        REQUIRE(remainingMask != 0);
        auto const nextBit = remainingMask & (~remainingMask + 1U);
        CHECK(nextBit == static_cast<std::uint32_t>(role));
        remainingMask &= ~nextBit;
      }

      CHECK(remainingMask == 0);
    }

    void checkAlacPackets(DecoderSession& decoder,
                          AlacFrameTestOptions const& options,
                          LayoutExpectation const& expected,
                          SampleEncoding encoding)
    {
      // Identity must be final at open, before the provider decodes any PCM.
      auto const info = decoder.streamInfo();
      checkAlacInfo(info, options, expected, encoding);
      REQUIRE(expected.frame.size() == frameBytes(info.outputFormat));
      auto packet = std::vector<std::uint8_t>{};

      for (std::uint32_t frameIndex = 0; frameIndex < 16; ++frameIndex)
      {
        packet.insert(packet.end(), expected.frame.begin(), expected.frame.end());
      }

      for (std::uint32_t pass = 0; pass < 2; ++pass)
      {
        CAPTURE(pass);

        for (std::uint32_t packetIndex = 0; packetIndex < options.packetCount; ++packetIndex)
        {
          CAPTURE(packetIndex);
          auto const blockRes = decoder.readNextBlock();
          REQUIRE(blockRes);
          CHECK(blockRes->frames == 16);
          CHECK(blockRes->firstFrameIndex == static_cast<std::uint64_t>(packetIndex) * 16U);
          CHECK(blockRes->endOfStream == (packetIndex + 1U == options.packetCount));
          CHECK(std::ranges::equal(blockRes->bytes, utility::bytes::view(std::span<std::uint8_t const>{packet})));
          CHECK(decoder.streamInfo() == info);
        }

        for (std::uint32_t repeat = 0; repeat < 2; ++repeat)
        {
          auto const eofRes = decoder.readNextBlock();
          REQUIRE(eofRes);
          CHECK(eofRes->endOfStream);
          CHECK(eofRes->frames == 0);
          CHECK(eofRes->bytes.empty());
          CHECK(decoder.streamInfo() == info);
        }

        if (pass == 0)
        {
          REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
          decoder.flush();
          CHECK(decoder.streamInfo() == info);
        }
      }
    }

    void checkAlacOpenPaths(AlacFrameTestOptions const& options,
                            LayoutExpectation const& expected,
                            SampleEncoding encoding = SampleEncoding::Signed16Le,
                            std::optional<SampleEncoding> optRequestedEncoding = std::nullopt)
    {
      auto const temp = ao::test::TempFile{buildAlacUncompressedFileBytes(options), ".m4a"};
      auto concretePtr = ao::test::requireValue(AlacDecoderSession::open(temp.path, optRequestedEncoding));
      auto factoryPtr = ao::test::requireValue(openDecoderSession(temp.path, optRequestedEncoding));
      REQUIRE(concretePtr);
      REQUIRE(factoryPtr);
      CHECK(concretePtr->streamInfo() == factoryPtr->streamInfo());
      checkAlacPackets(*concretePtr, options, expected, encoding);
      checkAlacPackets(*factoryPtr, options, expected, encoding);
    }
  } // namespace

  TEST_CASE("AlacDecoderSession - absent metadata identifies only mono and stereo without relabeling PCM",
            "[audio][integration][alac][decoder]")
  {
    using enum Speaker;
    auto const cases = std::vector<LayoutExpectation>{
      {0x4, {FrontCenter}, {0x52, 0x51}},
      {0x3, {FrontLeft, FrontRight}, {0x52, 0x51, 0x64, 0x63}},
      {0, {}, {0x52, 0x51, 0x64, 0x63, 0x76, 0x75}},
      {0, {}, {0x52, 0x51, 0x64, 0x63, 0x76, 0x75, 0xE8, 0xE8}},
      {0, {}, {0x52, 0x51, 0x64, 0x63, 0x76, 0x75, 0xE8, 0xE8, 0x2A, 0x29}},
      {0, {}, {0x52, 0x51, 0x64, 0x63, 0x76, 0x75, 0xE8, 0xE8, 0x2A, 0x29, 0xC4, 0xC4}},
      {0, {}, {0x52, 0x51, 0x64, 0x63, 0x76, 0x75, 0xE8, 0xE8, 0x2A, 0x29, 0xC4, 0xC4, 0x4E, 0x4D}},
      {0, {}, {0x52, 0x51, 0x64, 0x63, 0x76, 0x75, 0xE8, 0xE8, 0x2A, 0x29, 0xC4, 0xC4, 0x4E, 0x4D, 0xA0, 0xA0}},
    };

    for (std::size_t index = 0; index < cases.size(); ++index)
    {
      auto const channels = static_cast<std::uint8_t>(index + 1U);
      CAPTURE(channels);
      checkAlacOpenPaths({.channels = channels, .packetCount = 2}, cases[index]);
    }
  }

  TEST_CASE("AlacDecoderSession - named layouts canonicalize every supported channel count before publication",
            "[audio][integration][alac][decoder]")
  {
    using enum Speaker;
    struct NamedCase final
    {
      std::uint8_t channels;
      std::uint32_t tag;
      LayoutExpectation expected;
    };
    auto const cases = std::vector<NamedCase>{
      {1, 0x00640001, {0x4, {FrontCenter}, {0x52, 0x51}}},
      {2, 0x00650002, {0x3, {FrontLeft, FrontRight}, {0x52, 0x51, 0x64, 0x63}}},
      {3, 0x00720003, {0x7, {FrontLeft, FrontRight, FrontCenter}, {0x64, 0x63, 0x76, 0x75, 0x52, 0x51}}},
      {4,
       0x00740004,
       {0x107, {FrontLeft, FrontRight, FrontCenter, BackCenter}, {0x64, 0x63, 0x76, 0x75, 0x52, 0x51, 0xE8, 0xE8}}},
      {5,
       0x00780005,
       {0x37,
        {FrontLeft, FrontRight, FrontCenter, BackLeft, BackRight},
        {0x64, 0x63, 0x76, 0x75, 0x52, 0x51, 0xE8, 0xE8, 0x2A, 0x29}}},
      {6,
       0x007C0006,
       {0x3F,
        {FrontLeft, FrontRight, FrontCenter, LowFrequency, BackLeft, BackRight},
        {0x64, 0x63, 0x76, 0x75, 0x52, 0x51, 0xC4, 0xC4, 0xE8, 0xE8, 0x2A, 0x29}}},
      {7,
       0x008E0007,
       {0x13F,
        {FrontLeft, FrontRight, FrontCenter, LowFrequency, BackLeft, BackRight, BackCenter},
        {0x64, 0x63, 0x76, 0x75, 0x52, 0x51, 0x4E, 0x4D, 0xE8, 0xE8, 0x2A, 0x29, 0xC4, 0xC4}}},
      {8,
       0x007F0008,
       {0xFF,
        {FrontLeft, FrontRight, FrontCenter, LowFrequency, BackLeft, BackRight, FrontLeftOfCenter, FrontRightOfCenter},
        {0xE8, 0xE8, 0x2A, 0x29, 0x52, 0x51, 0xA0, 0xA0, 0xC4, 0xC4, 0x4E, 0x4D, 0x64, 0x63, 0x76, 0x75}}},
    };

    for (auto const& testCase : cases)
    {
      CAPTURE(testCase.channels, testCase.tag);
      checkAlacOpenPaths(
        {.channels = testCase.channels, .packetCount = 4, .sampleEntrySuffix = makeAlacChannelLayoutAtom(testCase.tag)},
        testCase.expected);
    }
  }

  TEST_CASE("AlacDecoderSession - permutation decisions remain session-local across sequential opens",
            "[audio][integration][alac][decoder]")
  {
    using enum Speaker;
    // Apple CoreAudioBaseTypes.h raw orders: MPEG 5.0 B is L R Ls Rs C;
    // MPEG 5.0 A is L R C Ls Rs. Each source slot keeps its literal S16 marker.
    auto const expectedB = LayoutExpectation{0x37,
                                             {FrontLeft, FrontRight, FrontCenter, BackLeft, BackRight},
                                             {0x52, 0x51, 0x64, 0x63, 0x2A, 0x29, 0x76, 0x75, 0xE8, 0xE8}};
    auto const expectedA = LayoutExpectation{0x37,
                                             {FrontLeft, FrontRight, FrontCenter, BackLeft, BackRight},
                                             {0x52, 0x51, 0x64, 0x63, 0x76, 0x75, 0xE8, 0xE8, 0x2A, 0x29}};

    // Each helper call destroys its sessions before the next distinct open;
    // this exercises per-open decisions, not an unsupported reopen-in-place API.
    checkAlacOpenPaths({.channels = 5,
                        .precisionBits = 16,
                        .packetCount = 2,
                        .sampleEntrySuffix = makeAlacChannelLayoutAtom(0x00760005)},
                       expectedB);
    checkAlacOpenPaths({.channels = 5,
                        .precisionBits = 16,
                        .packetCount = 2,
                        .sampleEntrySuffix = makeAlacChannelLayoutAtom(0x00750005)},
                       expectedA);
    checkAlacOpenPaths({.channels = 5, .precisionBits = 16, .packetCount = 2},
                       {0, {}, {0x52, 0x51, 0x64, 0x63, 0x76, 0x75, 0xE8, 0xE8, 0x2A, 0x29}});
    checkAlacOpenPaths({.channels = 5,
                        .precisionBits = 16,
                        .packetCount = 2,
                        .sampleEntrySuffix = makeAlacChannelLayoutAtom(0x00760005)},
                       expectedB);
  }

  TEST_CASE("AlacDecoderSession - generic bitmap and ordered descriptions preserve their declared three-slot roles",
            "[audio][integration][alac][decoder]")
  {
    using enum Speaker;
    auto options = AlacFrameTestOptions{.channels = 3, .packetCount = 3};
    auto expected = LayoutExpectation{};

    SECTION("Bitmap order is already ascending WAVE bit order and need not include center")
    {
      options.sampleEntrySuffix = makeAlacChannelLayoutAtom(0x00010000, 0xB);
      expected = {0xB, {FrontLeft, FrontRight, LowFrequency}, {0x52, 0x51, 0x64, 0x63, 0x76, 0x75}};
    }

    SECTION("Description array order names center then left then right")
    {
      auto const labels = std::to_array<std::uint32_t>({3, 1, 2});
      options.sampleEntrySuffix = makeAlacChannelLayoutAtom(0, 0, labels);
      expected = {0x7, {FrontLeft, FrontRight, FrontCenter}, {0x64, 0x63, 0x76, 0x75, 0x52, 0x51}};
    }

    checkAlacOpenPaths(options, expected);
  }

  TEST_CASE("AlacDecoderSession - slot normalization precedes lossless sixteen-bit widening",
            "[audio][integration][alac][decoder]")
  {
    using enum Speaker;
    checkAlacOpenPaths(
      {.channels = 4, .packetCount = 4, .sampleEntrySuffix = makeAlacChannelLayoutAtom(0x00740004)},
      {0x107,
       {FrontLeft, FrontRight, FrontCenter, BackCenter},
       {0x00, 0x00, 0x64, 0x63, 0x00, 0x00, 0x76, 0x75, 0x00, 0x00, 0x52, 0x51, 0x00, 0x00, 0xE8, 0xE8}},
      SampleEncoding::Signed32Le,
      SampleEncoding::Signed32Le);
  }

  TEST_CASE("AlacDecoderSession - packed twenty-four-bit normalization copies complete native samples",
            "[audio][integration][alac][decoder]")
  {
    using enum Speaker;
    checkAlacOpenPaths({.channels = 4,
                        .precisionBits = 24,
                        .packetCount = 4,
                        .sampleEntrySuffix = makeAlacChannelLayoutAtom(0x00740004)},
                       {0x107,
                        {FrontLeft, FrontRight, FrontCenter, BackCenter},
                        {0x02, 0x64, 0x63, 0x03, 0x76, 0x75, 0x01, 0x52, 0x51, 0xFC, 0xE7, 0xE8}},
                       SampleEncoding::Signed24PackedLe);
  }

  TEST_CASE("AlacDecoderSession - thirty-two-bit normalization retains low bits and negative slot markers",
            "[audio][integration][alac][decoder]")
  {
    using enum Speaker;
    checkAlacOpenPaths(
      {.channels = 8,
       .precisionBits = 32,
       .packetCount = 4,
       .sampleEntrySuffix = makeAlacChannelLayoutAtom(0x007F0008)},
      {0xFF,
       {FrontLeft, FrontRight, FrontCenter, LowFrequency, BackLeft, BackRight, FrontLeftOfCenter, FrontRightOfCenter},
       {0xFC, 0xFF, 0xE7, 0xE8, 0x05, 0x00, 0x2A, 0x29, 0x01, 0x00, 0x52, 0x51, 0xF8, 0xFF, 0x9F, 0xA0,
        0xFA, 0xFF, 0xC3, 0xC4, 0x07, 0x00, 0x4E, 0x4D, 0x02, 0x00, 0x64, 0x63, 0x03, 0x00, 0x76, 0x75}},
      SampleEncoding::Signed32Le);
  }

  TEST_CASE("AlacDecoderSession - twenty-bit surround normalization retains signed packed sample markers",
            "[audio][integration][alac][decoder]")
  {
    using enum Speaker;
    checkAlacOpenPaths(
      {.channels = 6,
       .precisionBits = 20,
       .packetCount = 4,
       .sampleEntrySuffix = makeAlacChannelLayoutAtom(0x007C0006)},
      {0x3F,
       {FrontLeft, FrontRight, FrontCenter, LowFrequency, BackLeft, BackRight},
       {0x20, 0x64, 0x63, 0x30, 0x76, 0x75, 0x10, 0x52, 0x51, 0xA0, 0xC3, 0xC4, 0xC0, 0xE7, 0xE8, 0x50, 0x2A, 0x29}},
      SampleEncoding::Signed24PackedLe);
  }

  TEST_CASE("AlacDecoderSession - twenty-bit normalization retains packed alignment and source precision",
            "[audio][integration][alac][decoder]")
  {
    using enum Speaker;
    checkAlacOpenPaths(
      {.channels = 3,
       .precisionBits = 20,
       .packetCount = 4,
       .sampleEntrySuffix = makeAlacChannelLayoutAtom(0x00720003)},
      {0x7, {FrontLeft, FrontRight, FrontCenter}, {0x20, 0x64, 0x63, 0x30, 0x76, 0x75, 0x10, 0x52, 0x51}},
      SampleEncoding::Signed24PackedLe);
  }
} // namespace ao::audio::test
