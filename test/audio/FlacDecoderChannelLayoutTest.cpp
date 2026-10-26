// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "FlacFrameTestSupport.h"
#include "lib/audio/DecoderFactory.h"
#include "lib/audio/FlacDecoderSession.h"
#include "test/TestFixtureSupport.h"
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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    constexpr auto kMarkers16 = std::array{
      std::to_array<std::int32_t>({16384, -8192, 4096, -2048, 1024, -512, 256, -128}),
      std::to_array<std::int32_t>({-16384, 8192, -4096, 2048, -1024, 512, -256, 128}),
      std::to_array<std::int32_t>({8192, -4096, 2048, -1024, 512, -256, 128, -64}),
    };
    constexpr auto kMarkers24 = std::array{
      std::to_array<std::int32_t>({4194305, -2097153, 1048578}),
      std::to_array<std::int32_t>({-4194305, 2097153, -1048578}),
      std::to_array<std::int32_t>({2097153, -1048578, 524289}),
    };

    // Literal interleaved LE bytes, independent of the FLAC subframe packer.
    // Every slot has a distinct signed marker; the middle frame changes all
    // slots, so stale or skipped PCM cannot masquerade as correct positioning.
    constexpr auto kExpected16 = std::array{
      std::to_array<std::uint8_t>(
        {0x00, 0x40, 0x00, 0xE0, 0x00, 0x10, 0x00, 0xF8, 0x00, 0x04, 0x00, 0xFE, 0x00, 0x01, 0x80, 0xFF}),
      std::to_array<std::uint8_t>(
        {0x00, 0xC0, 0x00, 0x20, 0x00, 0xF0, 0x00, 0x08, 0x00, 0xFC, 0x00, 0x02, 0x00, 0xFF, 0x80, 0x00}),
      std::to_array<std::uint8_t>(
        {0x00, 0x20, 0x00, 0xF0, 0x00, 0x08, 0x00, 0xFC, 0x00, 0x02, 0x00, 0xFF, 0x80, 0x00, 0xC0, 0xFF}),
    };

    std::vector<std::uint8_t> buildLayoutStream(std::uint8_t channels,
                                                std::span<std::string const> comments = {},
                                                bool hasCommentBlock = false,
                                                std::uint8_t bitsPerSample = 16)
    {
      auto bytes = buildFlacStreaminfoBytes(48, channels, bitsPerSample);

      if (hasCommentBlock)
      {
        appendFlacCommentBlock(bytes, comments);
      }

      for (std::uint32_t frameIndex = 0; frameIndex < 3U; ++frameIndex)
      {
        auto const markers = bitsPerSample == 24U ? std::span<std::int32_t const>{kMarkers24[frameIndex]}
                                                  : std::span<std::int32_t const>{kMarkers16[frameIndex]};
        auto const frame = buildFlacVerbatimFrameBytes(frameIndex, 48000, channels, bitsPerSample, markers);
        bytes.insert(bytes.end(), frame.begin(), frame.end());
      }

      return bytes;
    }

    void checkIdentity(DecodedStreamInfo const& info,
                       std::uint8_t channels,
                       std::uint32_t mask,
                       std::uint8_t precisionBits = 16,
                       SampleEncoding encoding = SampleEncoding::Signed16Le,
                       ChannelLayoutOrigin origin = ChannelLayoutOrigin::Identified)
    {
      CHECK(info.sourceFormat == SignalFormat{.sampleRate = 48000,
                                              .channels = channels,
                                              .precisionBits = precisionBits,
                                              .sampleKind = SampleKind::Integer,
                                              .speakerMask = mask});
      CHECK(info.outputFormat ==
            PcmFormat{.sampleRate = 48000, .channels = channels, .encoding = encoding, .speakerMask = mask});
      CHECK(info.sourceChannelLayout.origin == origin);
      CHECK(info.optTotalFrames == 48U);
      CHECK(info.codec == AudioCodec::Flac);
      CHECK_FALSE(info.isLossy);
    }

    void checkRepeatedFrame(PcmBlock const& block,
                            std::uint64_t firstFrameIndex,
                            std::uint32_t frames,
                            std::span<std::uint8_t const> expectedFrame)
    {
      CHECK(block.firstFrameIndex == firstFrameIndex);
      REQUIRE(block.frames == frames);
      REQUIRE(block.bytes.size() == frames * expectedFrame.size());
      CHECK_FALSE(block.endOfStream);

      for (std::size_t frame = 0; frame < frames; ++frame)
      {
        for (std::size_t byteIndex = 0; byteIndex < expectedFrame.size(); ++byteIndex)
        {
          CAPTURE(frame, byteIndex);
          CHECK(std::to_integer<std::uint8_t>(block.bytes[(frame * expectedFrame.size()) + byteIndex]) ==
                expectedFrame[byteIndex]);
        }
      }
    }

    void checkAllLayoutPcm(DecoderSession& decoder, std::uint8_t channels)
    {
      auto const info = decoder.streamInfo();

      for (std::size_t frameIndex = 0; frameIndex < 3U; ++frameIndex)
      {
        auto const blockRes = decoder.readNextBlock();
        REQUIRE(blockRes);
        checkRepeatedFrame(
          *blockRes,
          frameIndex * 16U,
          16,
          std::span<std::uint8_t const>{kExpected16[frameIndex]}.first(static_cast<std::size_t>(channels) * 2U));
        CHECK(decoder.streamInfo() == info);
      }

      for (std::uint32_t repeat = 0; repeat < 3U; ++repeat)
      {
        auto const endRes = decoder.readNextBlock();
        REQUIRE(endRes);
        CHECK(endRes->endOfStream);
        CHECK(endRes->frames == 0);
        CHECK(endRes->bytes.empty());
        CHECK(decoder.streamInfo() == info);
      }
    }

    void checkUnknownComments(std::span<std::string const> comments)
    {
      // A supported declaration on either side must never restore identity.
      for (std::uint8_t placement = 0; placement < 3U; ++placement)
      {
        CAPTURE(placement);
        auto declarations = std::vector<std::string>{comments.begin(), comments.end()};

        if (placement == 1U)
        {
          declarations.insert(declarations.begin(), "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3F");
        }
        else if (placement == 2U)
        {
          declarations.emplace_back("WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3F");
        }

        auto const temp = ao::test::TempFile{buildLayoutStream(6, declarations, true), ".flac"};
        auto concretePtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
        auto factoryPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
        checkIdentity(concretePtr->streamInfo(), 6, 0, 16, SampleEncoding::Signed16Le, ChannelLayoutOrigin::Unknown);
        CHECK(factoryPtr->streamInfo() == concretePtr->streamInfo());
        checkAllLayoutPcm(*concretePtr, 6);
        checkAllLayoutPcm(*factoryPtr, 6);
      }
    }
  } // namespace

  TEST_CASE("FlacDecoderSession - absent masks withhold only ambiguous five and six channel identities")
  {
    // Five/six-channel back-versus-side geometry needs an explicit declaration.
    // The other entries retain Aobus's selected format-default identities.
    constexpr auto kMasks = std::to_array<std::uint32_t>({4, 3, 7, 0x33, 0, 0, 0x70F, 0x63F});

    for (std::uint8_t channels = 1; channels <= 8U; ++channels)
    {
      CAPTURE(channels);
      auto const temp = ao::test::TempFile{buildLayoutStream(channels), ".flac"};
      auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
      auto factoryPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
      auto const mask = kMasks[channels - 1U];
      checkIdentity(decoderPtr->streamInfo(),
                    channels,
                    mask,
                    16,
                    SampleEncoding::Signed16Le,
                    mask == 0U ? ChannelLayoutOrigin::Unknown : ChannelLayoutOrigin::Identified);
      CHECK(factoryPtr->streamInfo() == decoderPtr->streamInfo());
      checkAllLayoutPcm(*decoderPtr, channels);
      checkAllLayoutPcm(*factoryPtr, channels);
    }
  }

  TEST_CASE("FlacDecoderSession - empty and unrelated comments cannot resolve ambiguous surround geometry")
  {
    auto const commentSets = std::vector<std::vector<std::string>>{
      {},
      {"TITLE=No channel declaration", "ARTIST=Fixture"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK_EXTRA=bad",
       "XWAVEFORMATEXTENSIBLE_CHANNEL_MASK=bad",
       "WAVEFORMATEXTENSIBLE_CHANNEL_MAS=bad"},
    };

    for (auto const& comments : commentSets)
    {
      for (std::uint8_t const channels : std::to_array<std::uint8_t>({2, 5, 6}))
      {
        CAPTURE(comments, channels);
        auto const temp = ao::test::TempFile{buildLayoutStream(channels, comments, true), ".flac"};
        auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
        checkIdentity(decoderPtr->streamInfo(),
                      channels,
                      channels == 2U ? 3U : 0U,
                      16,
                      SampleEncoding::Signed16Le,
                      channels == 2U ? ChannelLayoutOrigin::Identified : ChannelLayoutOrigin::Unknown);
        checkAllLayoutPcm(*decoderPtr, channels);
      }
    }
  }

  TEST_CASE("FlacDecoderSession - mask field names are matched exactly without trimming spaces")
  {
    struct Case final
    {
      std::string_view name;
      std::vector<std::string> comments;
      std::uint32_t mask;
    };

    auto const unknownComments = std::array{
      std::string{" WAVEFORMATEXTENSIBLE_CHANNEL_MASK=garbage"},
      std::string{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK =garbage"},
    };
    auto const known = std::string{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x60F"};

    for (auto const& unknown : unknownComments)
    {
      auto const cases = std::array{
        Case{"exact field absent", {unknown}, 0},
        Case{"exact field before unknown", {known, unknown}, 0x60F},
        Case{"exact field after unknown", {unknown, known}, 0x60F},
      };

      for (auto const& testCase : cases)
      {
        CAPTURE(unknown, testCase.name);
        auto const temp = ao::test::TempFile{buildLayoutStream(6, testCase.comments, true), ".flac"};
        auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
        checkIdentity(decoderPtr->streamInfo(),
                      6,
                      testCase.mask,
                      16,
                      SampleEncoding::Signed16Le,
                      testCase.mask == 0U ? ChannelLayoutOrigin::Unknown : ChannelLayoutOrigin::Identified);
        checkAllLayoutPcm(*decoderPtr, 6);
      }
    }
  }

  TEST_CASE("FlacDecoderSession - complete explicit identities preserve coded slot order at every channel count")
  {
    struct Case final
    {
      std::uint8_t channels;
      std::uint32_t mask;
      std::string_view comment;
    };

    constexpr auto kCases = std::array{
      Case{1, 0x4, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x4"},
      Case{1, 0x1, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x1"},
      Case{2, 0xC, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xC"},
      Case{3, 0x7, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x7"},
      Case{4, 0x33, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x33"},
      Case{5, 0x37, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x37"},
      Case{5, 0x607, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x607"},
      Case{6, 0x3F, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3F"},
      Case{6, 0x60F, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x60F"},
      Case{7, 0x70F, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x70F"},
      Case{8, 0x63F, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x63F"},
      Case{4, 0x603, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x603"},
      Case{4, 0x12104, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x12104"},
      Case{1, 0x20000, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x20000"},
    };

    for (auto const& testCase : kCases)
    {
      CAPTURE(testCase.comment);
      auto const comments = std::array{std::string{testCase.comment}};
      auto const temp = ao::test::TempFile{buildLayoutStream(testCase.channels, comments, true), ".flac"};
      auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
      auto factoryPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
      checkIdentity(decoderPtr->streamInfo(), testCase.channels, testCase.mask);
      CHECK(factoryPtr->streamInfo() == decoderPtr->streamInfo());
      checkAllLayoutPcm(*decoderPtr, testCase.channels);
      checkAllLayoutPcm(*factoryPtr, testCase.channels);
    }
  }

  TEST_CASE("FlacDecoderSession - mask text accepts ASCII key case and numeric hexadecimal spelling")
  {
    struct Case final
    {
      std::uint8_t channels;
      std::uint32_t mask;
      std::string_view comment;
    };

    constexpr auto kCases = std::array{
      Case{6, 0x3F, "waveformatextensible_channel_mask=0X3F"},
      Case{6, 0x3F, "WaVeFoRmAtExTeNsIbLe_ChAnNeL_MaSk=0x000000000000000000000000000003f"},
      Case{6, 0x3F, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x0000003F"},
      Case{6, 0xAF, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0XaF"},
      Case{6, 0xAF, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xAf"},
      Case{3, 0xB, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xB"},
      Case{2, 0xC, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xc"},
      Case{3, 0xD, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xd"},
      Case{3, 0xE, "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xE"},
    };

    for (auto const& testCase : kCases)
    {
      CAPTURE(testCase.comment);
      auto const comments = std::array{std::string{testCase.comment}};
      auto const temp = ao::test::TempFile{buildLayoutStream(testCase.channels, comments, true), ".flac"};
      auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
      checkIdentity(decoderPtr->streamInfo(), testCase.channels, testCase.mask);
      checkAllLayoutPcm(*decoderPtr, testCase.channels);
    }
  }

  TEST_CASE("FlacDecoderSession - numerically identical duplicate masks are accepted")
  {
    auto const comments = std::to_array<std::string>({
      "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3F",
      "TITLE=Between declarations",
      "waveformatextensible_channel_mask=0X000000000000000003f",
      "WaVeFoRmAtExTeNsIbLe_ChAnNeL_MaSk=0x0000003F",
    });
    auto const temp = ao::test::TempFile{buildLayoutStream(6, comments, true), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
    checkIdentity(decoderPtr->streamInfo(), 6, 0x3F);
    checkAllLayoutPcm(*decoderPtr, 6);
  }

  TEST_CASE("FlacDecoderSession - conflicting duplicate masks play with unknown identity in either order")
  {
    auto const first = std::string{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3F"};
    auto const second = std::string{"waveformatextensible_channel_mask=0X60F"};

    for (bool const reverse : {false, true})
    {
      CAPTURE(reverse);
      auto const comments =
        std::array{reverse ? second : first, std::string{"TITLE=Between declarations"}, reverse ? first : second};
      checkUnknownComments(comments);
    }
  }

  TEST_CASE("FlacDecoderSession - malformed recognized mask text plays with unknown identity")
  {
    auto const bareKey = std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK"});
    checkUnknownComments(bareKey);

    constexpr auto kValues = std::to_array<std::string_view>({
      "",       "3F",       "0x",    "0X",          "+0x3F",
      "-0x3F",  "0x+3F",    "0x-3F", " 0x3F",       "0x3F ",
      "\t0x3F", "0x3F\n",   "0x 3F", "0x3_F",       "0x3:F",
      "0x3G",   "0x3Fjunk", "0x3F=", "0x100000000", "0x000000000100000000",
    });

    for (auto const value : kValues)
    {
      CAPTURE(value);
      auto const comments = std::array{std::string{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK="} + std::string{value}};
      checkUnknownComments(comments);
    }
  }

  TEST_CASE("FlacDecoderSession - embedded NUL and suffix bytes withhold identity without refusing audio")
  {
    auto const values = std::to_array<std::string>(
      {std::string{"0x3F\0", 5}, std::string{"0x3F\0junk", 9}, std::string{"0x3\0F", 5}, std::string{"\0", 1}});

    for (auto const& value : values)
    {
      CAPTURE(value.size());
      auto const comments = std::array{std::string{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK="} + value};
      checkUnknownComments(comments);
    }
  }

  TEST_CASE("FlacDecoderSession - a malformed duplicate keeps identity unknown in either order")
  {
    auto const good = std::string{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3F"};

    for (auto const badValue : std::to_array<std::string_view>({"", "0x3Fjunk", "0x100000000"}))
    {
      auto const bad = std::string{"waveformatextensible_channel_mask="} + std::string{badValue};

      for (bool const reverse : {false, true})
      {
        CAPTURE(badValue, reverse);
        auto const comments = std::array{reverse ? bad : good, reverse ? good : bad};
        checkUnknownComments(comments);
      }
    }
  }

  TEST_CASE("FlacDecoderSession - explicit incomplete or unassigned masks play with unknown identity")
  {
    // A six-channel stream must not inherit WAV's zero override or silently
    // trim an excessive/highest bit to manufacture a complete identity.
    constexpr auto kValues = std::to_array<std::string_view>({
      "0x0",
      "0X000000000000000000000",
      "0x3",
      "0x7F",
      "0x4003F",
      "0x40000",
      "0x80000000",
      "0x8000003F",
      "0xFFFFFFFF",
    });

    for (auto const value : kValues)
    {
      CAPTURE(value);
      auto const comments = std::array{std::string{"WAVEFORMATEXTENSIBLE_CHANNEL_MASK="} + std::string{value}};
      checkUnknownComments(comments);
    }
  }

  TEST_CASE("FlacDecoderSession - explicit identity survives block reads seek flush and stable EOF")
  {
    auto const comments = std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x12104"});
    auto const temp = ao::test::TempFile{buildLayoutStream(4, comments, true), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
    auto& decoder = *decoderPtr;
    auto const info = decoder.streamInfo();
    checkIdentity(info, 4, 0x12104);

    auto const firstRes = decoder.readNextBlock();
    REQUIRE(firstRes);
    checkRepeatedFrame(*firstRes, 0, 16, std::span<std::uint8_t const>{kExpected16[0]}.first(8));
    CHECK(decoder.streamInfo() == info);

    // A healthy flush must retain the unread middle frame, despite read-ahead.
    decoder.flush();
    CHECK(decoder.streamInfo() == info);
    auto const middleRes = decoder.readNextBlock();
    REQUIRE(middleRes);
    checkRepeatedFrame(*middleRes, 16, 16, std::span<std::uint8_t const>{kExpected16[1]}.first(8));

    REQUIRE(decoder.seekFrame(21));
    CHECK(decoder.streamInfo() == info);
    // Flush also preserves already decoded, unread seek-target PCM.
    decoder.flush();
    auto const soughtRes = decoder.readNextBlock();
    REQUIRE(soughtRes);
    checkRepeatedFrame(*soughtRes, 21, 11, std::span<std::uint8_t const>{kExpected16[1]}.first(8));
    CHECK(decoder.streamInfo() == info);

    auto const lastRes = decoder.readNextBlock();
    REQUIRE(lastRes);
    checkRepeatedFrame(*lastRes, 32, 16, std::span<std::uint8_t const>{kExpected16[2]}.first(8));

    for (std::uint32_t repeat = 0; repeat < 3U; ++repeat)
    {
      auto const endRes = decoder.readNextBlock();
      REQUIRE(endRes);
      CHECK(endRes->endOfStream);
      CHECK(endRes->frames == 0);
      CHECK(endRes->bytes.empty());
      CHECK(decoder.streamInfo() == info);
    }

    REQUIRE(decoder.seekFrame(0));
    CHECK(decoder.streamInfo() == info);
    checkAllLayoutPcm(decoder, 4);
  }

  TEST_CASE("FlacDecoderSession - unknown identity retains literal widened PCM through seek flush and EOF")
  {
    // S16 markers widened to S32; these literals are independent of conversion.
    constexpr auto kExpected32 = std::array{
      std::to_array<std::uint8_t>({0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xE0}),
      std::to_array<std::uint8_t>({0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x20}),
      std::to_array<std::uint8_t>({0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0xF0}),
    };
    auto const comments = std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x7"});
    auto const temp = ao::test::TempFile{buildLayoutStream(2, comments, true), ".flac"};

    auto decoders = std::array<std::unique_ptr<DecoderSession>, 2>{
      ao::test::requireValue(FlacDecoderSession::open(temp.path, SampleEncoding::Signed32Le)),
      ao::test::requireValue(openDecoderSession(temp.path, SampleEncoding::Signed32Le)),
    };
    CHECK(decoders[0]->streamInfo() == decoders[1]->streamInfo());

    for (auto const& decoderPtr : decoders)
    {
      auto const info = decoderPtr->streamInfo();
      checkIdentity(info, 2, 0, 16, SampleEncoding::Signed32Le, ChannelLayoutOrigin::Unknown);
      CHECK(decoderPtr->supportsFrameSeek());
      auto const firstRes = decoderPtr->readNextBlock();
      REQUIRE(firstRes);
      checkRepeatedFrame(*firstRes, 0, 16, kExpected32[0]);
      decoderPtr->flush();
      CHECK(decoderPtr->streamInfo() == info);
      auto const middleRes = decoderPtr->readNextBlock();
      REQUIRE(middleRes);
      checkRepeatedFrame(*middleRes, 16, 16, kExpected32[1]);
      CHECK(decoderPtr->streamInfo() == info);
      REQUIRE(decoderPtr->seekFrame(21));
      CHECK(decoderPtr->streamInfo() == info);
      decoderPtr->flush();
      CHECK(decoderPtr->streamInfo() == info);
      auto const soughtRes = decoderPtr->readNextBlock();
      REQUIRE(soughtRes);
      checkRepeatedFrame(*soughtRes, 21, 11, kExpected32[1]);
      CHECK(decoderPtr->streamInfo() == info);
      auto const lastRes = decoderPtr->readNextBlock();
      REQUIRE(lastRes);
      checkRepeatedFrame(*lastRes, 32, 16, kExpected32[2]);
      CHECK(decoderPtr->streamInfo() == info);

      for (std::uint32_t repeat = 0; repeat < 3U; ++repeat)
      {
        auto const endRes = decoderPtr->readNextBlock();
        REQUIRE(endRes);
        CHECK(endRes->endOfStream);
        CHECK(endRes->frames == 0U);
        CHECK(endRes->bytes.empty());
        CHECK(decoderPtr->streamInfo() == info);
      }

      REQUIRE(decoderPtr->seekFrame(0));
      CHECK(decoderPtr->streamInfo() == info);
      auto const restartedRes = decoderPtr->readNextBlock();
      REQUIRE(restartedRes);
      checkRepeatedFrame(*restartedRes, 0, 16, kExpected32[0]);
      CHECK(decoderPtr->streamInfo() == info);
    }
  }

  TEST_CASE("FlacDecoderSession - all precision-preserving 16-bit outputs keep explicit identity and bytes")
  {
    struct Case final
    {
      SampleEncoding encoding;
      std::vector<std::uint8_t> expectedFrame;
    };

    auto const cases = std::array{
      Case{SampleEncoding::Signed16Le, {0x00, 0x40, 0x00, 0xE0, 0x00, 0x10}},
      Case{SampleEncoding::Signed24PackedLe, {0x00, 0x00, 0x40, 0x00, 0x00, 0xE0, 0x00, 0x00, 0x10}},
      Case{SampleEncoding::Signed24In32Le, {0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xE0, 0xFF, 0x00, 0x00, 0x10, 0x00}},
      Case{SampleEncoding::Signed32Le, {0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xE0, 0x00, 0x00, 0x00, 0x10}},
      // IEEE-754 bits for exactly +1/2, -1/4, +1/8; no production conversion oracle.
      Case{SampleEncoding::Float32Le, {0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x80, 0xBE, 0x00, 0x00, 0x00, 0x3E}},
    };
    auto const comments = std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x12004"});
    auto const temp = ao::test::TempFile{buildLayoutStream(3, comments, true), ".flac"};

    for (auto const& testCase : cases)
    {
      CAPTURE(testCase.encoding);
      auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, testCase.encoding));
      auto const info = decoderPtr->streamInfo();
      checkIdentity(info, 3, 0x12004, 16, testCase.encoding);
      auto const blockRes = decoderPtr->readNextBlock();
      REQUIRE(blockRes);
      checkRepeatedFrame(*blockRes, 0, 16, testCase.expectedFrame);
      CHECK(decoderPtr->streamInfo() == info);
    }
  }

  TEST_CASE("FlacDecoderSession - 24-bit precision and explicit identity survive supported outputs")
  {
    struct Case final
    {
      std::optional<SampleEncoding> optEncoding;
      SampleEncoding expectedEncoding;
      std::vector<std::uint8_t> expectedFrame;
    };

    auto const cases = std::array{
      Case{std::nullopt, SampleEncoding::Signed24PackedLe, {0x01, 0x00, 0x40, 0xFF, 0xFF, 0xDF, 0x02, 0x00, 0x10}},
      Case{SampleEncoding::Signed24PackedLe,
           SampleEncoding::Signed24PackedLe,
           {0x01, 0x00, 0x40, 0xFF, 0xFF, 0xDF, 0x02, 0x00, 0x10}},
      Case{SampleEncoding::Signed24In32Le,
           SampleEncoding::Signed24In32Le,
           {0x01, 0x00, 0x40, 0x00, 0xFF, 0xFF, 0xDF, 0xFF, 0x02, 0x00, 0x10, 0x00}},
      Case{SampleEncoding::Signed32Le,
           SampleEncoding::Signed32Le,
           {0x00, 0x01, 0x00, 0x40, 0x00, 0xFF, 0xFF, 0xDF, 0x00, 0x02, 0x00, 0x10}},
      // Exact binary32 encodings of 4194305/8388608, -2097153/8388608,
      // and 1048578/8388608, including the source's low significant bits.
      Case{SampleEncoding::Float32Le,
           SampleEncoding::Float32Le,
           {0x02, 0x00, 0x00, 0x3F, 0x04, 0x00, 0x80, 0xBE, 0x10, 0x00, 0x00, 0x3E}},
    };
    auto const comments = std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x12004"});
    auto const temp = ao::test::TempFile{buildLayoutStream(3, comments, true, 24), ".flac"};

    for (auto const& testCase : cases)
    {
      CAPTURE(testCase.expectedEncoding, testCase.optEncoding.has_value());
      auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, testCase.optEncoding));
      auto const info = decoderPtr->streamInfo();
      checkIdentity(info, 3, 0x12004, 24, testCase.expectedEncoding);
      auto const blockRes = decoderPtr->readNextBlock();
      REQUIRE(blockRes);
      checkRepeatedFrame(*blockRes, 0, 16, testCase.expectedFrame);
      CHECK(decoderPtr->streamInfo() == info);
    }

    auto const losingRes = FlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le);
    REQUIRE_FALSE(losingRes);
    CHECK(losingRes.error().code == Error::Code::NotSupported);
  }

  TEST_CASE("FlacDecoderSession - precision and geometry failures retain priority over layout admission")
  {
    auto bytes = buildFlacStreaminfoBytes(48, 2, 24);
    auto const comments = std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x1"});
    appendFlacCommentBlock(bytes, comments);

    SECTION("Precision-losing output keeps the existing adapter diagnostic")
    {
      auto const temp = ao::test::TempFile{bytes, ".flac"};
      auto const openRes = FlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le);
      REQUIRE_FALSE(openRes);
      CHECK(openRes.error().code == Error::Code::NotSupported);
      CHECK(openRes.error().message.contains("Requested decoder PCM encoding loses source precision"));
    }

    SECTION("Zero sample rate fails as geometry rather than unsupported layout")
    {
      // STREAMINFO's 20-bit sample rate occupies bytes 18, 19 and the high
      // nibble of byte 20; preserve the channel count and precision bits.
      REQUIRE(bytes.size() > 20U);
      bytes[18] = 0;
      bytes[19] = 0;
      bytes[20] &= 0x0FU;
      auto const temp = ao::test::TempFile{bytes, ".flac"};

      // This pins the public geometry-vs-layout failure code, not proof that
      // the metadata callback was reached: the provider may reject it first.
      for (auto const optEncoding :
           std::array<std::optional<SampleEncoding>, 2>{std::nullopt, SampleEncoding::Signed24PackedLe})
      {
        CAPTURE(optEncoding.has_value());
        auto const openRes = FlacDecoderSession::open(temp.path, optEncoding);
        REQUIRE_FALSE(openRes);
        CHECK(openRes.error().code == Error::Code::DecodeFailed);
      }
    }
  }

  TEST_CASE("FlacDecoderSession - factory and concrete opens agree on ordinary layout admission")
  {
    auto const validCommentSets = std::vector<std::vector<std::string>>{
      {},
      {"TITLE=Default"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x603"},
    };

    for (std::size_t index = 0; index < validCommentSets.size(); ++index)
    {
      auto const temp = ao::test::TempFile{buildLayoutStream(4, validCommentSets[index], index != 0U), ".flac"};

      for (auto const optEncoding :
           std::array<std::optional<SampleEncoding>, 2>{std::nullopt, SampleEncoding::Signed32Le})
      {
        CAPTURE(index, optEncoding.has_value());
        auto concretePtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, optEncoding));
        auto factoryPtr = ao::test::requireValue(openDecoderSession(temp.path, optEncoding));
        checkIdentity(factoryPtr->streamInfo(),
                      4,
                      index == 2U ? 0x603 : 0x33,
                      16,
                      optEncoding.value_or(SampleEncoding::Signed16Le));
        CHECK(concretePtr->streamInfo() == factoryPtr->streamInfo());
        auto const concreteRes = concretePtr->readNextBlock();
        auto const factoryRes = factoryPtr->readNextBlock();
        REQUIRE(concreteRes);
        REQUIRE(factoryRes);
        CHECK(concreteRes->firstFrameIndex == factoryRes->firstFrameIndex);
        CHECK(concreteRes->frames == factoryRes->frames);
        REQUIRE(concreteRes->bytes.size() == factoryRes->bytes.size());
        CHECK(std::equal(concreteRes->bytes.begin(), concreteRes->bytes.end(), factoryRes->bytes.begin()));
      }
    }

    auto const unknownCommentSets = std::vector<std::vector<std::string>>{
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x0"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3Fjunk"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3F", "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x60F"},
    };

    for (auto const& comments : unknownCommentSets)
    {
      auto const temp = ao::test::TempFile{buildLayoutStream(6, comments, true), ".flac"};

      for (auto const optEncoding :
           std::array<std::optional<SampleEncoding>, 2>{std::nullopt, SampleEncoding::Signed16Le})
      {
        CAPTURE(comments, optEncoding.has_value());
        auto concretePtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, optEncoding));
        auto factoryPtr = ao::test::requireValue(openDecoderSession(temp.path, optEncoding));
        checkIdentity(concretePtr->streamInfo(), 6, 0, 16, SampleEncoding::Signed16Le, ChannelLayoutOrigin::Unknown);
        CHECK(factoryPtr->streamInfo() == concretePtr->streamInfo());
        checkAllLayoutPcm(*concretePtr, 6);
        checkAllLayoutPcm(*factoryPtr, 6);
      }
    }
  }
} // namespace ao::audio::test
