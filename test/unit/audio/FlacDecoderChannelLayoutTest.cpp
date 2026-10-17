// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/audio/DecoderFactory.h"
#include "lib/audio/FlacDecoderSession.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/FlacFrameTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
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
    // Independent RFC 9639 / WAVE speaker-bit oracle. These names identify the
    // literal marker slots below, not native delivery or a production helper.
    enum class FlacRole : std::uint32_t
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
      std::uint8_t channels;
      std::uint32_t mask;
      std::vector<FlacRole> roles;
    };

    using CommentBlocks = std::vector<std::vector<std::string>>;

    constexpr auto kFirstMarkers = std::to_array<std::int32_t>({4097, 8194, 12291, 16388, 20485, 24582, 28679, -32760});
    constexpr auto kSecondMarkers =
      std::to_array<std::int32_t>({-8063, -12158, -16253, -20348, -24443, -28538, -32633, -3960});
    constexpr auto kFirstPcm = std::to_array<std::uint8_t>(
      {0x01, 0x10, 0x02, 0x20, 0x03, 0x30, 0x04, 0x40, 0x05, 0x50, 0x06, 0x60, 0x07, 0x70, 0x08, 0x80});
    constexpr auto kSecondPcm = std::to_array<std::uint8_t>(
      {0x81, 0xE0, 0x82, 0xD0, 0x83, 0xC0, 0x84, 0xB0, 0x85, 0xA0, 0x86, 0x90, 0x87, 0x80, 0x88, 0xF0});

    void appendLe32(std::vector<std::uint8_t>& bytes, std::uint32_t const value)
    {
      for (std::uint32_t shift = 0; shift < 32; shift += 8)
      {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
      }
    }

    void appendCommentBlock(std::vector<std::uint8_t>& bytes,
                            std::vector<std::string> const& comments,
                            bool const isLast)
    {
      auto body = std::vector<std::uint8_t>{};
      appendLe32(body, 0); // Empty vendor string.
      appendLe32(body, static_cast<std::uint32_t>(comments.size()));

      for (auto const& comment : comments)
      {
        appendLe32(body, static_cast<std::uint32_t>(comment.size()));
        body.insert(body.end(), comment.begin(), comment.end());
      }

      REQUIRE(body.size() <= 0xFFFFFFU);
      bytes.push_back(isLast ? 0x84 : 0x04);
      bytes.push_back(static_cast<std::uint8_t>((body.size() >> 16U) & 0xFFU));
      bytes.push_back(static_cast<std::uint8_t>((body.size() >> 8U) & 0xFFU));
      bytes.push_back(static_cast<std::uint8_t>(body.size() & 0xFFU));
      bytes.insert(bytes.end(), body.begin(), body.end());
    }

    std::vector<std::uint8_t> buildLayoutStream(std::uint8_t const channels,
                                                CommentBlocks const& blocks = {},
                                                std::uint8_t const precisionBits = 16,
                                                std::span<std::int32_t const> const firstMarkers = kFirstMarkers,
                                                std::span<std::int32_t const> const secondMarkers = kSecondMarkers)
    {
      REQUIRE((channels >= 1 && channels <= 8));
      REQUIRE((precisionBits == 16 || precisionBits == 24));
      auto bytes = buildFlacStreaminfoBytes(48);
      // The independent shared builder fixes stereo/16-bit STREAMINFO. Change
      // only its three channel bits and five precision bits, retaining 48 kHz.
      bytes[20] =
        static_cast<std::uint8_t>((bytes[20] & 0xF0U) | ((channels - 1U) << 1U) | ((precisionBits - 1U) >> 4U));
      bytes[21] = static_cast<std::uint8_t>((bytes[21] & 0x0FU) | (((precisionBits - 1U) & 0x0FU) << 4U));

      if (!blocks.empty())
      {
        bytes[4] = 0; // STREAMINFO is no longer the last metadata block.

        for (std::size_t index = 0; index < blocks.size(); ++index)
        {
          appendCommentBlock(bytes, blocks[index], index + 1U == blocks.size());
        }
      }

      for (std::uint32_t frameIndex = 0; frameIndex < 3; ++frameIndex)
      {
        auto const frame = buildFlacVerbatimFrameBytes(
          frameIndex, 48000, channels, precisionBits, frameIndex == 1 ? secondMarkers : firstMarkers);
        bytes.insert(bytes.end(), frame.begin(), frame.end());
      }

      return bytes;
    }

    void checkLayout(DecodedStreamInfo const& info,
                     LayoutCase const& expected,
                     ChannelLayoutOrigin const origin,
                     std::optional<std::uint32_t> const optDeclaration,
                     SampleEncoding const encoding = SampleEncoding::Signed16Le,
                     std::uint8_t const precisionBits = 16)
    {
      CHECK(info.sourceFormat.sampleRate == 48000);
      CHECK(info.outputFormat.sampleRate == 48000);
      CHECK(info.sourceFormat.channels == expected.channels);
      CHECK(info.outputFormat.channels == expected.channels);
      CHECK(info.sourceFormat.precisionBits == precisionBits);
      CHECK(info.sourceFormat.speakerMask == expected.mask);
      CHECK(info.outputFormat.speakerMask == expected.mask);
      CHECK(info.outputFormat.encoding == encoding);
      CHECK(info.sourceChannelLayout.origin == origin);
      CHECK(info.sourceChannelLayout.optDeclaredMask == optDeclaration);
      CHECK_FALSE(info.isLossy);
      CHECK(info.duration == std::chrono::milliseconds{1});
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

    // Narrow companion to checkLayout: the generic five/six-channel default
    // does not establish a complete exact WAVE set. This model deliberately
    // underidentifies it; zero masks do not mean every coded role is unknown.
    // Assert that interpretation against the same 48 kHz / 16-bit fixture.
    void checkUnidentifiedLayout(DecodedStreamInfo const& info, std::uint8_t const channels)
    {
      CHECK(info.sourceFormat.sampleRate == 48000);
      CHECK(info.outputFormat.sampleRate == 48000);
      CHECK(info.sourceFormat.channels == channels);
      CHECK(info.outputFormat.channels == channels);
      CHECK(info.sourceFormat.precisionBits == 16);
      CHECK(info.sourceFormat.speakerMask == 0);
      CHECK(info.outputFormat.speakerMask == 0);
      CHECK(info.outputFormat.encoding == SampleEncoding::Signed16Le);
      CHECK(info.sourceChannelLayout.origin == ChannelLayoutOrigin::Unknown);
      CHECK_FALSE(info.sourceChannelLayout.optDeclaredMask);
      CHECK_FALSE(info.isLossy);
      CHECK(info.duration == std::chrono::milliseconds{1});
    }

    void checkPcmBlock(PcmBlock const& block,
                       std::uint64_t const firstFrameIndex,
                       std::uint32_t const frames,
                       std::span<std::uint8_t const> const pcmFrame)
    {
      CHECK(block.firstFrameIndex == firstFrameIndex);
      CHECK(block.frames == frames);
      CHECK_FALSE(block.endOfStream);
      REQUIRE(block.bytes.size() == static_cast<std::size_t>(frames) * pcmFrame.size());
      auto const expected = utility::bytes::view(pcmFrame);

      for (std::size_t frame = 0; frame < frames; ++frame)
      {
        CHECK(std::ranges::equal(block.bytes.subspan(frame * expected.size(), expected.size()), expected));
      }
    }

    void checkEof(DecoderSession& decoder)
    {
      for (std::size_t repeat = 0; repeat < 2; ++repeat)
      {
        auto const endRes = decoder.readNextBlock();
        REQUIRE(endRes);
        CHECK(endRes->endOfStream);
        CHECK(endRes->frames == 0);
        CHECK(endRes->bytes.empty());
      }
    }

    void checkPlayback(DecoderSession& decoder,
                       std::span<std::uint8_t const> const firstPcm,
                       std::span<std::uint8_t const> const secondPcm)
    {
      auto const info = decoder.streamInfo();
      auto const firstRes = decoder.readNextBlock();
      REQUIRE(firstRes);
      checkPcmBlock(*firstRes, 0, 16, firstPcm);
      decoder.flush();
      auto const middleRes = decoder.readNextBlock();
      REQUIRE(middleRes);
      checkPcmBlock(*middleRes, 16, 16, secondPcm);
      auto const lastRes = decoder.readNextBlock();
      REQUIRE(lastRes);
      checkPcmBlock(*lastRes, 32, 16, firstPcm);
      checkEof(decoder);

      // 1 ms is one past the 48 samples. Seek clamps to sample 47, preserving
      // the already decoded final sample across flush without changing roles.
      REQUIRE(decoder.seek(std::chrono::milliseconds{1}));
      decoder.flush();
      auto const tailRes = decoder.readNextBlock();
      REQUIRE(tailRes);
      checkPcmBlock(*tailRes, 47, 1, firstPcm);
      checkEof(decoder);
      REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
      auto const replayRes = decoder.readNextBlock();
      REQUIRE(replayRes);
      checkPcmBlock(*replayRes, 0, 16, firstPcm);
      CHECK(decoder.streamInfo() == info);
    }
  } // namespace

  TEST_CASE("FlacDecoderSession - publishes every precise representable default before decoding any PCM",
            "[audio][integration][flac][decoder]")
  {
    using enum FlacRole;
    // Five and six channels have no precise WAVE default: their generic
    // surround slots stay unidentified in the dedicated case below instead
    // of borrowing a back/side mask.
    auto const cases = std::vector<LayoutCase>{
      {1, 0x4, {FrontCenter}},
      {2, 0x3, {FrontLeft, FrontRight}},
      {3, 0x7, {FrontLeft, FrontRight, FrontCenter}},
      {4, 0x33, {FrontLeft, FrontRight, BackLeft, BackRight}},
      {7, 0x70F, {FrontLeft, FrontRight, FrontCenter, LowFrequency, BackCenter, SideLeft, SideRight}},
      {8, 0x63F, {FrontLeft, FrontRight, FrontCenter, LowFrequency, BackLeft, BackRight, SideLeft, SideRight}},
    };

    for (auto const& expected : cases)
    {
      for (auto const& blocks : std::vector<CommentBlocks>{
             {},
             {{}},
             {{"TITLE=unrelated", "WAVEFORMATEXTENSIBLE_CHANNEL_MASK_EXTRA=0x0", "OTHER=0xFFFFFFFF"}},
             // Repeated blocks without any mask keep the default too.
             {{"TITLE=unrelated"}, {"OTHER=0xFFFFFFFF", "WAVEFORMATEXTENSIBLE_CHANNEL_MASK_EXTRA=0x0"}},
             {{}, {"TITLE=first"}, {"OTHER=0xFFFFFFFF"}, {}}})
      {
        CAPTURE(expected.channels, blocks.size());
        auto const temp = ao::test::TempFile{buildLayoutStream(expected.channels, blocks), ".flac"};
        auto decoderPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
        auto const info = decoderPtr->streamInfo();
        checkLayout(info, expected, ChannelLayoutOrigin::FormatDefault, std::nullopt);
        CHECK(signalFormat(info.outputFormat) == info.sourceFormat);
        auto const frameBytes = static_cast<std::size_t>(expected.channels) * 2U;
        checkPlayback(*decoderPtr,
                      std::span<std::uint8_t const>{kFirstPcm}.first(frameBytes),
                      std::span<std::uint8_t const>{kSecondPcm}.first(frameBytes));
      }
    }
  }

  TEST_CASE("FlacDecoderSession - undeclared five and six channel streams stay unidentified with zero speaker masks",
            "[audio][integration][flac][decoder]")
  {
    for (auto const channels : std::to_array<std::uint8_t>({5, 6}))
    {
      for (auto const& blocks : std::vector<CommentBlocks>{
             {},
             {{}},
             {{"TITLE=unrelated", "WAVEFORMATEXTENSIBLE_CHANNEL_MASK_EXTRA=0x0", "OTHER=0xFFFFFFFF"}},
             // Repeated blocks without any mask keep the stream unidentified.
             {{"TITLE=unrelated"}, {"OTHER=0xFFFFFFFF", "WAVEFORMATEXTENSIBLE_CHANNEL_MASK_EXTRA=0x0"}},
             {{}, {"TITLE=first"}, {"OTHER=0xFFFFFFFF"}, {}}})
      {
        CAPTURE(channels, blocks.size());
        auto const temp = ao::test::TempFile{buildLayoutStream(channels, blocks), ".flac"};
        auto decoderPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));

        // The finalized unidentified layout is available before any PCM read.
        auto const info = decoderPtr->streamInfo();
        checkUnidentifiedLayout(info, channels);
        CHECK(signalFormat(info.outputFormat) == info.sourceFormat);
        auto const frameBytes = static_cast<std::size_t>(channels) * 2U;
        checkPlayback(*decoderPtr,
                      std::span<std::uint8_t const>{kFirstPcm}.first(frameBytes),
                      std::span<std::uint8_t const>{kSecondPcm}.first(frameBytes));
      }
    }
  }

  TEST_CASE(
    "FlacDecoderSession - complete alternate declarations preserve all assigned speaker roles and literal slots",
    "[audio][integration][flac][decoder]")
  {
    using enum FlacRole;
    auto const cases = std::vector<LayoutCase>{
      {1, 0x1, {FrontLeft}},
      {2, 0x30, {BackLeft, BackRight}},
      {3, 0x2000C, {FrontCenter, LowFrequency, TopBackRight}},
      {4, 0x107, {FrontLeft, FrontRight, FrontCenter, BackCenter}},
      {5, 0xF40, {FrontLeftOfCenter, BackCenter, SideLeft, SideRight, TopCenter}},
      {6, 0x3F000, {TopFrontLeft, TopFrontCenter, TopFrontRight, TopBackLeft, TopBackCenter, TopBackRight}},
      {7, 0x7F0, {BackLeft, BackRight, FrontLeftOfCenter, FrontRightOfCenter, BackCenter, SideLeft, SideRight}},
      {8,
       0x3FC00,
       {SideRight, TopCenter, TopFrontLeft, TopFrontCenter, TopFrontRight, TopBackLeft, TopBackCenter, TopBackRight}},
    };

    for (auto const& expected : cases)
    {
      CAPTURE(expected.channels, expected.mask);
      auto const comments = CommentBlocks{
        {"TITLE=before", std::format("WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x{:X}", expected.mask), "TITLE=after"}};
      auto const temp = ao::test::TempFile{buildLayoutStream(expected.channels, comments), ".flac"};
      auto decoderPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
      checkLayout(decoderPtr->streamInfo(), expected, ChannelLayoutOrigin::Declared, expected.mask);
      auto const frameBytes = static_cast<std::size_t>(expected.channels) * 2U;
      checkPlayback(*decoderPtr,
                    std::span<std::uint8_t const>{kFirstPcm}.first(frameBytes),
                    std::span<std::uint8_t const>{kSecondPcm}.first(frameBytes));
    }
  }

  TEST_CASE("FlacDecoderSession - complete surround declarations identify five and six channel streams",
            "[audio][integration][flac][decoder]")
  {
    using enum FlacRole;
    auto const cases = std::vector<LayoutCase>{
      // The standard back-or-side layouts for five and six channels.
      {5, 0x37, {FrontLeft, FrontRight, FrontCenter, BackLeft, BackRight}},
      {5, 0x607, {FrontLeft, FrontRight, FrontCenter, SideLeft, SideRight}},
      {6, 0x3F, {FrontLeft, FrontRight, FrontCenter, LowFrequency, BackLeft, BackRight}},
      {6, 0x60F, {FrontLeft, FrontRight, FrontCenter, LowFrequency, SideLeft, SideRight}},
      // Arbitrary complete sets that mix back and side speakers; they are
      // valid declarations, not the standard back-or-side layouts above.
      {5, 0x703, {FrontLeft, FrontRight, BackCenter, SideLeft, SideRight}},
      {6, 0x633, {FrontLeft, FrontRight, BackLeft, BackRight, SideLeft, SideRight}},
    };

    for (auto const& expected : cases)
    {
      auto const declaration = std::format("WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x{:X}", expected.mask);

      // The declaration identifies the stream whether it is the only comment
      // or arrives only after empty and unrelated metadata blocks.
      for (auto const& blocks : std::vector<CommentBlocks>{{{declaration}}, {{}, {"TITLE=middle"}, {declaration}}})
      {
        CAPTURE(expected.channels, expected.mask, blocks.size());
        auto const temp = ao::test::TempFile{buildLayoutStream(expected.channels, blocks), ".flac"};
        auto decoderPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
        checkLayout(decoderPtr->streamInfo(), expected, ChannelLayoutOrigin::Declared, expected.mask);
        auto const frameBytes = static_cast<std::size_t>(expected.channels) * 2U;
        checkPlayback(*decoderPtr,
                      std::span<std::uint8_t const>{kFirstPcm}.first(frameBytes),
                      std::span<std::uint8_t const>{kSecondPcm}.first(frameBytes));
      }
    }
  }

  TEST_CASE("FlacDecoderSession - accepts case variants zero padding and equivalent duplicate declarations",
            "[audio][integration][flac][decoder]")
  {
    using enum FlacRole;
    auto const expected = LayoutCase{2, 0xC, {FrontCenter, LowFrequency}};
    auto const cases = std::vector<CommentBlocks>{
      {{"waveformatextensible_channel_mask=0Xc"}},
      {{"WaVeFoRmAtExTeNsIbLe_ChAnNeL_MaSk=0x0000000000000000000000000000000C"}},
      {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x" + std::string(4096, '0') + "c"}},
      {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xc", "TITLE=middle", "waveformatextensible_channel_mask=0X000C"}},
      // One mask declared again by a later block, in case and padding variants.
      {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0Xc"},
       {"TITLE=unrelated"},
       {"waveformatextensible_channel_mask=0x0000000C"}},
      // Same-block duplicates interleaved with unrelated entries and blocks.
      {{"TITLE=first",
        "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xc",
        "TITLE=middle",
        "waveformatextensible_channel_mask=0X000C",
        "OTHER=0xFFFFFFFF"},
       {"TITLE=unrelated"}},
    };

    for (auto const& blocks : cases)
    {
      CAPTURE(blocks.size());
      auto const temp = ao::test::TempFile{buildLayoutStream(2, blocks), ".flac"};
      auto decoderPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
      checkLayout(decoderPtr->streamInfo(), expected, ChannelLayoutOrigin::Declared, 0xC);
      checkPlayback(*decoderPtr,
                    std::span<std::uint8_t const>{kFirstPcm}.first(4),
                    std::span<std::uint8_t const>{kSecondPcm}.first(4));
    }
  }

  TEST_CASE("FlacDecoderSession - source admission refuses zero partial reserved wildcard and excess masks",
            "[audio][integration][flac][decoder]")
  {
    struct RefusalCase final
    {
      std::uint8_t channels;
      std::uint32_t mask;
      std::string_view reason;
    };
    auto const cases = std::to_array<RefusalCase>({
      {1, 0x0, "explicit zero"},
      {2, 0x0, "explicit zero"},
      // Five and six channels have no default mask to fall back on; explicit
      // zero and partial masks stay refusals instead of becoming a
      // declaration waiver for the newly uncertain defaults.
      {5, 0x0, "explicit zero"},
      {6, 0x0, "explicit zero"},
      {2, 0x1, "partially"},
      {4, 0x3, "partially"},
      {8, 0x7F, "partially"},
      {5, 0x1, "partially"},
      {6, 0x3, "partially"},
      {1, 0x40000, "reserved"},
      {2, 0x40001, "reserved"},
      {2, 0x40040000, "reserved"},
      {1, 0x80000000, "wildcard"},
      {2, 0xFFFFFFFF, "wildcard"},
      {1, 0x3, "excess"},
      {2, 0x3F, "excess"},
      {8, 0x3FFFF, "excess"},
    });

    for (auto const& expected : cases)
    {
      CAPTURE(expected.channels, expected.mask);
      auto const blocks =
        CommentBlocks{{"TITLE=unrelated", std::format("WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x{:X}", expected.mask)}};
      auto const temp = ao::test::TempFile{buildLayoutStream(expected.channels, blocks), ".flac"};
      auto const openRes = openDecoderSession(temp.path, std::nullopt);
      REQUIRE_FALSE(openRes);
      CHECK(openRes.error().code == Error::Code::NotSupported);
      CHECK(openRes.error().message.contains("FLAC channel layout"));
      CHECK(openRes.error().message.contains(std::format("0x{:08X}", expected.mask)));
      CHECK(openRes.error().message.contains(std::format("{} channels", expected.channels)));
      CHECK(openRes.error().message.contains("entry 2"));
      CHECK(openRes.error().message.contains(expected.reason));
    }
  }

  TEST_CASE("FlacDecoderSession - malformed known declarations are contextual errors rather than missing metadata",
            "[audio][integration][flac][decoder]")
  {
    struct MalformedCase final
    {
      std::string value;
      std::string_view reason;
    };
    auto embeddedNulValue = std::string{"0x30"};
    embeddedNulValue.push_back('\0');
    embeddedNulValue.append("ignored");
    REQUIRE(embeddedNulValue.size() == 12);

    auto const cases = std::vector<MalformedCase>{
      {"", "requires a 0x prefix"},
      {"0x", "requires a 0x prefix"},
      {"30", "requires a 0x prefix"},
      {"0b11", "requires a 0x prefix"},
      {" 0x30", "requires a 0x prefix"},
      {"0x30 ", "non-hexadecimal"},
      {"+0x30", "requires a 0x prefix"},
      {"-0x30", "requires a 0x prefix"},
      {"0x+30", "non-hexadecimal"},
      {"0x-30", "non-hexadecimal"},
      {"0x3g", "non-hexadecimal"},
      {"0x30suffix", "non-hexadecimal"},
      {"0x30\n", "non-hexadecimal"},
      {"0x30=ignored", "non-hexadecimal"},
      {"0x100000000", "uint32"},
      {"0x00000000100000000", "uint32"},
      {embeddedNulValue, "non-hexadecimal"},
      {"0x\xFF", "non-hexadecimal"},
      {"0x" + std::string(4096, 'F'), "uint32"},
    };

    for (auto const& expected : cases)
    {
      CAPTURE(expected.value.size(), expected.reason);
      auto const blocks = CommentBlocks{{"TITLE=unrelated", "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=" + expected.value}};
      auto const temp = ao::test::TempFile{buildLayoutStream(2, blocks), ".flac"};
      auto const openRes = openDecoderSession(temp.path, std::nullopt);
      REQUIRE_FALSE(openRes);
      CHECK(openRes.error().code == Error::Code::NotSupported);
      CHECK(openRes.error().message.contains("FLAC channel layout"));
      CHECK(openRes.error().message.contains("2 channels"));
      CHECK(openRes.error().message.contains("entry 2"));
      CHECK(openRes.error().message.contains("prefix hex"));
      CHECK(openRes.error().message.contains(expected.reason));
      CHECK(openRes.error().message.size() < 512);
      CHECK(openRes.error().location.line() != 0);
      CHECK(std::string_view{openRes.error().location.file_name()}.contains("FlacDecoderSession.cpp"));
    }

    auto const temp = ao::test::TempFile{buildLayoutStream(2, {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK"}}), ".flac"};
    auto const openRes = openDecoderSession(temp.path, std::nullopt);
    REQUIRE_FALSE(openRes);
    CHECK(openRes.error().code == Error::Code::NotSupported);
    CHECK(openRes.error().message.contains("separator"));
  }

  TEST_CASE("FlacDecoderSession - conflicting invalid duplicates within one comment block cannot override admission",
            "[audio][integration][flac][decoder]")
  {
    struct ConflictCase final
    {
      CommentBlocks blocks;
      std::string_view reason;
    };
    auto const cases = std::vector<ConflictCase>{
      {{{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3", "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x30"}}, "conflicting"},
      {{{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x30", "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=bad"}}, "requires a 0x prefix"},
      {{{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=bad", "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x30"}}, "requires a 0x prefix"},
      {{{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3z", "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=bad"}}, "non-hexadecimal"},
      // Repeated blocks are tolerated below: equivalent declarations across
      // blocks and unrelated blocks without a mask are positive cases now.
    };

    for (auto const& expected : cases)
    {
      CAPTURE(expected.reason);
      auto const temp = ao::test::TempFile{buildLayoutStream(2, expected.blocks), ".flac"};
      auto const directRes = FlacDecoderSession::open(temp.path, std::nullopt);
      auto const factoryRes = openDecoderSession(temp.path, std::nullopt);
      REQUIRE_FALSE(directRes);
      REQUIRE_FALSE(factoryRes);
      CHECK(factoryRes.error().code == Error::Code::NotSupported);
      CHECK(factoryRes.error().message.contains(expected.reason));
      CHECK(factoryRes.error().message == directRes.error().message);
      CHECK(factoryRes.error().location.line() == directRes.error().location.line());
    }
  }

  TEST_CASE("FlacDecoderSession - tolerated repeated comment blocks preserve one declaration or the default",
            "[audio][integration][flac][decoder]")
  {
    using enum FlacRole;
    struct PersistenceCase final
    {
      std::string_view name;
      CommentBlocks blocks;
      LayoutCase expected;
      ChannelLayoutOrigin origin;
      std::optional<std::uint32_t> optDeclaration;
    };
    auto const cases = std::vector<PersistenceCase>{
      // Only a later block declares; empty and unrelated blocks come first.
      {"later declaration",
       {{}, {"TITLE=middle"}, {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x30"}},
       {2, 0x30, {BackLeft, BackRight}},
       ChannelLayoutOrigin::Declared,
       0x30},
      // The first declaration survives later unrelated blocks.
      {"retained declaration",
       {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x30"}, {"TITLE=unrelated"}, {"OTHER=0xFFFFFFFF"}},
       {2, 0x30, {BackLeft, BackRight}},
       ChannelLayoutOrigin::Declared,
       0x30},
      // Equivalent declarations in two blocks persist as one layout.
      {"equivalent blocks",
       {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3"}, {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3"}},
       {2, 0x3, {FrontLeft, FrontRight}},
       ChannelLayoutOrigin::Declared,
       0x3},
      // Repeated blocks without any mask keep the normative default.
      {"unrelated blocks",
       {{"TITLE=first"}, {"TITLE=second"}, {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK_EXTRA=0x0"}},
       {2, 0x3, {FrontLeft, FrontRight}},
       ChannelLayoutOrigin::FormatDefault,
       std::nullopt},
    };

    for (auto const& expected : cases)
    {
      CAPTURE(expected.name);
      auto const temp = ao::test::TempFile{buildLayoutStream(2, expected.blocks), ".flac"};
      auto decoderPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
      auto const info = decoderPtr->streamInfo();
      checkLayout(info, expected.expected, expected.origin, expected.optDeclaration);
      auto const directPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
      CHECK(directPtr->streamInfo() == info);
      checkPlayback(*decoderPtr,
                    std::span<std::uint8_t const>{kFirstPcm}.first(4),
                    std::span<std::uint8_t const>{kSecondPcm}.first(4));
    }
  }

  TEST_CASE(
    "FlacDecoderSession - cross-block mask conflicts and malformed declarations refuse admission without fallback",
    "[audio][integration][flac][decoder]")
  {
    struct CrossBlockCase final
    {
      std::string_view name;
      CommentBlocks blocks;
      // Empty reason: the refusal and its owning context are the oracle, since
      // a later invalid mask may read as a conflicting re-declaration or as
      // the unchanged per-entry value policy.
      std::string_view reason;
      std::string_view context;
    };
    auto embeddedNulValue = std::string{"0x30"};
    embeddedNulValue.push_back('\0');
    embeddedNulValue.append("ignored");

    auto const cases = std::vector<CrossBlockCase>{
      // Two different valid declarations conflict in either order.
      {"later conflict 0x3 then 0x30",
       {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3"}, {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x30"}},
       "conflicting",
       "Vorbis comment block 2, entry 1"},
      {"later conflict 0x30 then 0x3",
       {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x30"}, {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3"}},
       "conflicting",
       "Vorbis comment block 2, entry 1"},
      // A malformed known value refuses even after one valid declaration.
      {"later bad syntax",
       {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x30"}, {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=bad"}},
       "requires a 0x prefix",
       "Vorbis comment block 2, entry 1"},
      {"later overflow",
       {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x30"}, {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x100000000"}},
       "uint32",
       "Vorbis comment block 2, entry 1"},
      {"later embedded NUL",
       {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x30"}, {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=" + embeddedNulValue}},
       "non-hexadecimal",
       "Vorbis comment block 2, entry 1"},
      // An early malformed declaration is not cured by a later valid one.
      {"early bad not cured",
       {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=bad"}, {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x30"}},
       "requires a 0x prefix",
       "Vorbis comment block 1, entry 1"},
      // Later invalid declarations refuse instead of falling back to the
      // earlier declaration or the default.
      {"later zero",
       {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x30"}, {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x0"}},
       "",
       "Vorbis comment block 2, entry 1"},
      {"later partial",
       {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x30"}, {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x1"}},
       "",
       "Vorbis comment block 2, entry 1"},
      {"later reserved",
       {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x30"}, {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x40000"}},
       "",
       "Vorbis comment block 2, entry 1"},
      {"later wildcard",
       {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x30"}, {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xFFFFFFFF"}},
       "",
       "Vorbis comment block 2, entry 1"},
      {"later excess",
       {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x30"}, {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3F"}},
       "",
       "Vorbis comment block 2, entry 1"},
      // The per-entry value policy applies unchanged to a later block.
      {"zero in later block",
       {{"TITLE=unrelated"}, {"TITLE=x", "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x0"}},
       "explicit zero",
       "Vorbis comment block 2, entry 2"},
      // A stored invalid declaration keeps the first declaration's context,
      // also after later blocks with equivalent or unrelated comments.
      {"first declaration retained",
       {{"TITLE=unrelated", "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x0"},
        {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x000", "TITLE=after"}},
       "explicit zero",
       "Vorbis comment block 1, entry 2"},
      {"first declaration before unrelated blocks",
       {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x0"}, {"TITLE=unrelated"}, {"OTHER=0xFFFFFFFF"}},
       "explicit zero",
       "Vorbis comment block 1, entry 1"},
      // The first syntax failure owns the message; a later huge value cannot
      // replace it, and a huge owning value stays bounded in the message.
      {"first failure before huge comment",
       {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=bad"}, {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x" + std::string(4096, 'F')}},
       "requires a 0x prefix",
       "Vorbis comment block 1, entry 1"},
      {"huge owning failure",
       {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x" + std::string(4096, 'F')}, {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=bad"}},
       "uint32",
       "Vorbis comment block 1, entry 1"},
    };

    for (auto const& expected : cases)
    {
      CAPTURE(expected.name);
      auto const temp = ao::test::TempFile{buildLayoutStream(2, expected.blocks), ".flac"};
      auto const directRes = FlacDecoderSession::open(temp.path, std::nullopt);
      auto const factoryRes = openDecoderSession(temp.path, std::nullopt);
      REQUIRE_FALSE(directRes);
      REQUIRE_FALSE(factoryRes);
      CHECK(factoryRes.error().code == Error::Code::NotSupported);
      CHECK(factoryRes.error().message.contains(expected.context));

      if (!expected.reason.empty())
      {
        CHECK(factoryRes.error().message.contains(expected.reason));
      }

      CHECK(factoryRes.error().message.size() < 512);
      CHECK(factoryRes.error().location.line() != 0);
      CHECK(std::string_view{factoryRes.error().location.file_name()}.contains("FlacDecoderSession.cpp"));
      CHECK(factoryRes.error().message == directRes.error().message);
      CHECK(factoryRes.error().location.line() == directRes.error().location.line());
    }
  }

  TEST_CASE("FlacDecoderSession - all lossless output encodings retain finalized declared identity and literal bytes",
            "[audio][integration][flac][decoder]")
  {
    using enum FlacRole;
    auto const layout = LayoutCase{4, 0x107, {FrontLeft, FrontRight, FrontCenter, BackCenter}};
    // +.25, +.5, -.5, -.25 in source slot order, without a slot permutation.
    constexpr auto kMarkers16 = std::to_array<std::int32_t>({8192, 16384, -16384, -8192});
    constexpr auto kMarkers24 = std::to_array<std::int32_t>({2097152, 4194304, -4194304, -2097152});
    struct AdaptationCase final
    {
      SampleEncoding encoding;
      std::vector<std::uint8_t> pcm;
    };
    auto const cases = std::vector<AdaptationCase>{
      {SampleEncoding::Signed16Le, {0x00, 0x20, 0x00, 0x40, 0x00, 0xC0, 0x00, 0xE0}},
      {SampleEncoding::Signed24PackedLe, {0x00, 0x00, 0x20, 0x00, 0x00, 0x40, 0x00, 0x00, 0xC0, 0x00, 0x00, 0xE0}},
      {SampleEncoding::Signed24In32Le,
       {0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xC0, 0xFF, 0x00, 0x00, 0xE0, 0xFF}},
      {SampleEncoding::Signed32Le,
       {0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0xE0}},
      {SampleEncoding::Float32Le,
       {0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x80, 0xBE}},
    };

    for (auto const precisionBits : std::to_array<std::uint8_t>({16, 24}))
    {
      auto const markers =
        precisionBits == 16 ? std::span<std::int32_t const>{kMarkers16} : std::span<std::int32_t const>{kMarkers24};
      auto const temp = ao::test::TempFile{
        buildLayoutStream(4, {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x107"}}, precisionBits, markers, markers), ".flac"};

      for (auto const& expected : cases)
      {
        CAPTURE(precisionBits, expected.encoding);

        if (precisionBits == 24 && expected.encoding == SampleEncoding::Signed16Le)
        {
          auto const openRes = openDecoderSession(temp.path, expected.encoding);
          REQUIRE_FALSE(openRes);
          CHECK(openRes.error().code == Error::Code::NotSupported);
          CHECK(openRes.error().message.contains("loses source precision"));
          continue;
        }

        auto decoderPtr = ao::test::requireValue(openDecoderSession(temp.path, expected.encoding));
        checkLayout(
          decoderPtr->streamInfo(), layout, ChannelLayoutOrigin::Declared, 0x107, expected.encoding, precisionBits);
        checkPlayback(*decoderPtr, expected.pcm, expected.pcm);
      }
    }
  }

  TEST_CASE("FlacDecoderSession - a seek before the first read retains finalized declaration precedence",
            "[audio][integration][flac][decoder]")
  {
    using enum FlacRole;
    auto const temp = ao::test::TempFile{buildLayoutStream(2, {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x30"}}), ".flac"};
    auto decoderPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
    auto const info = decoderPtr->streamInfo();
    checkLayout(info, {2, 0x30, {BackLeft, BackRight}}, ChannelLayoutOrigin::Declared, 0x30);
    REQUIRE(decoderPtr->seek(std::chrono::milliseconds{1}));
    CHECK(decoderPtr->streamInfo() == info);
    auto const tailRes = decoderPtr->readNextBlock();
    REQUIRE(tailRes);
    checkPcmBlock(*tailRes, 47, 1, std::span<std::uint8_t const>{kFirstPcm}.first(4));
    checkEof(*decoderPtr);
    REQUIRE(decoderPtr->seek(std::chrono::milliseconds{0}));
    checkPlayback(*decoderPtr,
                  std::span<std::uint8_t const>{kFirstPcm}.first(4),
                  std::span<std::uint8_t const>{kSecondPcm}.first(4));
    CHECK(decoderPtr->streamInfo() == info);
  }

  TEST_CASE("FlacDecoderSession - declared identity is finalized without reading a contradicting first frame",
            "[audio][integration][flac][decoder]")
  {
    using enum FlacRole;
    auto bytes = buildLayoutStream(4, {{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x107"}});
    // Keep metadata, replace audio with a valid 24-bit first frame, contrary
    // to the declared 16-bit geometry. Successful open must not decode it.
    auto const metadataSize = bytes.size() - (3U * buildFlacVerbatimFrameBytes(0, 48000, 4, 16, kFirstMarkers).size());
    bytes.resize(metadataSize);
    auto const frame = buildFlacVerbatimFrameBytes(0, 48000, 4, 24, kFirstMarkers);
    bytes.insert(bytes.end(), frame.begin(), frame.end());
    auto const temp = ao::test::TempFile{bytes, ".flac"};
    auto decoderPtr = ao::test::requireValue(openDecoderSession(temp.path, SampleEncoding::Signed32Le));
    auto const info = decoderPtr->streamInfo();
    checkLayout(info,
                {4, 0x107, {FrontLeft, FrontRight, FrontCenter, BackCenter}},
                ChannelLayoutOrigin::Declared,
                0x107,
                SampleEncoding::Signed32Le);
    auto const firstRes = decoderPtr->readNextBlock();
    REQUIRE_FALSE(firstRes);
    CHECK(firstRes.error().code == Error::Code::DecodeFailed);
    CHECK(firstRes.error().message.contains("24 bits per sample"));
    auto const repeatedRes = decoderPtr->readNextBlock();
    REQUIRE_FALSE(repeatedRes);
    CHECK(repeatedRes.error().message == firstRes.error().message);
    CHECK(decoderPtr->streamInfo() == info);
  }
} // namespace ao::audio::test
