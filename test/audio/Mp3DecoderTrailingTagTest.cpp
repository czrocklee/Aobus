// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

// Regression tests for bounded trailing-tag support in Mp3DecoderSession:
// a recognized [Lyrics3v2][APEv2][ID3v1] suffix is cropped only after a full
// native scan witnesses its start at a complete indexed frame's end.

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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    // Synthetic MPEG1 Layer III fixture, as probed in Mp3DecoderChannelLayoutTest:
    // one 417-byte frame decodes to 1152 silent S16 PCM frames.
    constexpr std::size_t kTailTestMpegFrameBytes = 417;
    constexpr std::size_t kTailTestId3v1Bytes = 128;
    constexpr std::uint32_t kTailTestSampleRate = 44100;
    // A second synthetic geometry: MPEG1 Layer III at 32 kbit/s and 48000 Hz
    // gives 96-byte frames, so five frames decode to a clean 120 ms extent.
    constexpr std::uint32_t kTailTestWideSampleRate = 48000;
    constexpr std::size_t kTailTestWideMpegFrameBytes = 96;
    // A fake MPEG2.5 Layer III frame at 8 kbit/s and 12000 Hz spans 48 bytes;
    // it only ever appears inside tag values and ID3v1 bodies.
    constexpr std::size_t kTailTestFakeMpeg25FrameBytes = 48;
    constexpr std::uint64_t kTailTestPcmFramesPerMpegFrame = 1152;
    constexpr std::uint64_t kTailTestFiveFramePcmFrames = 5U * kTailTestPcmFramesPerMpegFrame;
    constexpr std::size_t kTailTestLateDualFrameCount = 1001;
    constexpr std::uint64_t kTailTestLateDualPcmFrames = kTailTestLateDualFrameCount * kTailTestPcmFramesPerMpegFrame;

    // One 32-byte APEv2 header/footer part: magic, version, tag size, item
    // count, flags, and eight reserved zero bytes.
    constexpr std::size_t kTailTestApeTagPartBytes = 32;
    constexpr std::size_t kTailTestApeVersionOffset = 8;
    constexpr std::size_t kTailTestApeSizeOffset = 12;
    constexpr std::size_t kTailTestApeCountOffset = 16;
    constexpr std::size_t kTailTestApeFlagsOffset = 20;
    constexpr std::size_t kTailTestApeReservedOffset = 24;
    constexpr std::uint32_t kTailTestApeVersion2000 = 2000;
    constexpr std::uint32_t kTailTestApeVersion1000 = 1000;
    constexpr std::uint32_t kTailTestApeFooterHasHeaderFlag = 0x80000000U;
    // An optional header must carry all footer flags plus the is-header bit.
    constexpr std::uint32_t kTailTestApeIsHeaderFlag = 0x20000000U;
    // The read-only bit is valid on footer flags alongside the header-present
    // bit, and on item flags as well.
    constexpr std::uint32_t kTailTestApeReadOnlyFlag = 0x00000001U;

    // Lyrics3v2: "LYRICSBEGIN", fields (three uppercase chars, five decimal
    // size digits, sized value), then a six-digit total length and
    // "LYRICS200"; the length excludes those final fifteen bytes.
    constexpr std::size_t kTailTestLyricsBeginBytes = 11;
    constexpr std::size_t kTailTestLyricsSizeDigits = 5;
    constexpr std::size_t kTailTestLyricsLengthDigits = 6;
    constexpr std::size_t kTailTestLyricsEndBytes = 9;

    enum class TailTestChannelMode : std::uint8_t
    {
      Stereo = 0,
      DualChannel = 2,
      Mono = 3,
    };

    struct TailTestModeCase final
    {
      TailTestChannelMode mode;
      std::uint8_t channels = 0;
      std::uint32_t speakerMask = 0;
    };

    constexpr auto kTailTestStereoMode =
      TailTestModeCase{.mode = TailTestChannelMode::Stereo, .channels = 2, .speakerMask = 0x3};
    constexpr auto kTailTestMonoMode =
      TailTestModeCase{.mode = TailTestChannelMode::Mono, .channels = 1, .speakerMask = 0x4};

    struct TailTestSessions final
    {
      std::unique_ptr<Mp3DecoderSession> concretePtr;
      std::unique_ptr<DecoderSession> factoryPtr;
    };

    struct TailTestApeItem final
    {
      std::string key;
      std::vector<std::uint8_t> value;
      std::uint32_t flags = 0;
    };

    struct TailTestLyricsField final
    {
      std::string id;
      std::string value;
    };

    enum class TailTestApeLayout : std::uint8_t
    {
      FooterOnly,
      HeaderAndFooter,
    };

    std::vector<std::uint8_t> tailTestAsciiBytes(std::string_view const text)
    {
      auto bytes = std::vector<std::uint8_t>{};

      for (char const ch : text)
      {
        bytes.push_back(static_cast<std::uint8_t>(ch));
      }

      return bytes;
    }

    void appendTailTestAscii(std::vector<std::uint8_t>& data, std::string_view const text)
    {
      for (char const ch : text)
      {
        data.push_back(static_cast<std::uint8_t>(ch));
      }
    }

    void appendTailTestU32Le(std::vector<std::uint8_t>& data, std::uint32_t const value)
    {
      for (std::uint32_t shift = 0; shift < 32U; shift += 8U)
      {
        data.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
      }
    }

    std::uint32_t readTailTestU32Le(std::vector<std::uint8_t> const& data, std::size_t const offset)
    {
      return static_cast<std::uint32_t>(data[offset]) | (static_cast<std::uint32_t>(data[offset + 1]) << 8U) |
             (static_cast<std::uint32_t>(data[offset + 2]) << 16U) |
             (static_cast<std::uint32_t>(data[offset + 3]) << 24U);
    }

    void overwriteTailTestU32Le(std::vector<std::uint8_t>& data, std::size_t const offset, std::uint32_t const value)
    {
      for (std::uint32_t shift = 0; shift < 32U; shift += 8U)
      {
        data[offset + (shift / 8U)] = static_cast<std::uint8_t>((value >> shift) & 0xFFU);
      }
    }

    void overwriteTailTestDecimalDigits(std::vector<std::uint8_t>& data,
                                        std::size_t const offset,
                                        std::uint64_t const value,
                                        std::size_t const width)
    {
      auto remaining = value;

      for (std::size_t digitIndex = width; digitIndex-- > 0;)
      {
        data[offset + digitIndex] = static_cast<std::uint8_t>('0' + (remaining % 10U));
        remaining /= 10U;
      }
    }

    void appendTailTestDecimalDigits(std::vector<std::uint8_t>& data,
                                     std::uint64_t const value,
                                     std::size_t const width)
    {
      auto const offset = data.size();
      data.resize(offset + width, static_cast<std::uint8_t>('0'));
      overwriteTailTestDecimalDigits(data, offset, value, width);
    }

    std::vector<std::uint8_t> buildTailTestMpegFrame(TailTestChannelMode const mode)
    {
      auto frame = std::vector<std::uint8_t>(kTailTestMpegFrameBytes, 0U);
      frame[0] = 0xFF;
      frame[1] = 0xFB; // MPEG1 Layer III, no CRC.
      frame[2] = 0x90; // 128 kbit/s, 44100 Hz.
      frame[3] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(mode) << 6U);

      return frame;
    }

    std::vector<std::uint8_t> buildTailTestFrameRun(TailTestChannelMode const mode, std::size_t const frameCount)
    {
      auto data = std::vector<std::uint8_t>{};

      for (std::size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
      {
        auto const frame = buildTailTestMpegFrame(mode);
        data.insert(data.end(), frame.begin(), frame.end());
      }

      return data;
    }

    // MPEG1 Layer III at 32 kbit/s and 48000 Hz: 96-byte frames that decode
    // to 1152 silent PCM frames each, like the shared 417-byte fixture.
    std::vector<std::uint8_t> buildTailTestWideMpegFrame(TailTestChannelMode const mode)
    {
      auto frame = std::vector<std::uint8_t>(kTailTestWideMpegFrameBytes, 0U);
      frame[0] = 0xFF;
      frame[1] = 0xFB; // MPEG1 Layer III, no CRC.
      frame[2] = 0x14; // 32 kbit/s, 48000 Hz.
      frame[3] = static_cast<std::uint8_t>(static_cast<std::uint8_t>(mode) << 6U);

      return frame;
    }

    std::vector<std::uint8_t> buildTailTestWideFrameRun(TailTestChannelMode const mode, std::size_t const frameCount)
    {
      auto data = std::vector<std::uint8_t>{};

      for (std::size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
      {
        auto const frame = buildTailTestWideMpegFrame(mode);
        data.insert(data.end(), frame.begin(), frame.end());
      }

      return data;
    }

    // A fake MPEG2.5 Layer III frame at 8 kbit/s, 12000 Hz, mono: 48 bytes
    // that must never be decoded as audio.
    std::vector<std::uint8_t> buildTailTestFakeMpeg25Frame()
    {
      auto frame = std::vector<std::uint8_t>(kTailTestFakeMpeg25FrameBytes, 0x55U);
      frame[0] = 0xFF;
      frame[1] = 0xE3; // MPEG2.5 Layer III, no CRC.
      frame[2] = 0x14; // 8 kbit/s, 12000 Hz.
      frame[3] = 0xC0; // Mono mode.

      return frame;
    }

    void appendTailTestId3v1Tag(std::vector<std::uint8_t>& data)
    {
      auto const tagOffset = data.size();
      data.resize(tagOffset + kTailTestId3v1Bytes, 0U);
      data[tagOffset] = static_cast<std::uint8_t>('T');
      data[tagOffset + 1] = static_cast<std::uint8_t>('A');
      data[tagOffset + 2] = static_cast<std::uint8_t>('G');
    }

    std::vector<TailTestApeItem> tailTestDefaultApeItems()
    {
      return {{.key = "Artist", .value = tailTestAsciiBytes("Trailing Tag Artist")},
              {.key = "Cover", .value = std::vector<std::uint8_t>(96, 0xB7U), .flags = 2U}};
    }

    void appendTailTestApeTagPart(std::vector<std::uint8_t>& data,
                                  std::uint32_t const tagSize,
                                  std::uint32_t const itemCount,
                                  std::uint32_t const flags)
    {
      appendTailTestAscii(data, "APETAGEX");
      appendTailTestU32Le(data, kTailTestApeVersion2000);
      appendTailTestU32Le(data, tagSize);
      appendTailTestU32Le(data, itemCount);
      appendTailTestU32Le(data, flags);
      data.resize(data.size() + 8, 0U); // Reserved zero bytes.
    }

    // The tag size counts the item bytes plus the 32-byte footer and excludes
    // an optional leading header, matching the APEv2 convention used by the
    // trailing-tag trimming in lib/media/file/mpeg/File.cpp.
    void appendTailTestApeTag(std::vector<std::uint8_t>& data,
                              std::vector<TailTestApeItem> const& items,
                              TailTestApeLayout const layout = TailTestApeLayout::FooterOnly,
                              std::uint32_t const extraFooterFlags = 0U)
    {
      auto itemBytes = std::vector<std::uint8_t>{};

      for (TailTestApeItem const& item : items)
      {
        appendTailTestU32Le(itemBytes, static_cast<std::uint32_t>(item.value.size()));
        appendTailTestU32Le(itemBytes, item.flags);
        appendTailTestAscii(itemBytes, item.key);
        itemBytes.push_back(0U); // NUL-terminated key.
        itemBytes.insert(itemBytes.end(), item.value.begin(), item.value.end());
      }

      auto const tagSize = static_cast<std::uint32_t>(itemBytes.size() + kTailTestApeTagPartBytes);
      auto const footerFlags =
        (layout == TailTestApeLayout::HeaderAndFooter ? kTailTestApeFooterHasHeaderFlag : 0U) | extraFooterFlags;

      if (layout == TailTestApeLayout::HeaderAndFooter)
      {
        appendTailTestApeTagPart(
          data, tagSize, static_cast<std::uint32_t>(items.size()), footerFlags | kTailTestApeIsHeaderFlag);
      }

      data.insert(data.end(), itemBytes.begin(), itemBytes.end());
      appendTailTestApeTagPart(data, tagSize, static_cast<std::uint32_t>(items.size()), footerFlags);
    }

    std::size_t tailTestApeFooterOffset(std::vector<std::uint8_t> const& data)
    {
      return data.size() - kTailTestApeTagPartBytes;
    }

    std::size_t tailTestApeTagSize(std::vector<std::uint8_t> const& data)
    {
      return readTailTestU32Le(data, tailTestApeFooterOffset(data) + kTailTestApeSizeOffset);
    }

    std::size_t tailTestApeItemsOffset(std::vector<std::uint8_t> const& data)
    {
      return tailTestApeFooterOffset(data) - tailTestApeTagSize(data) + kTailTestApeTagPartBytes;
    }

    std::size_t tailTestApeHeaderOffset(std::vector<std::uint8_t> const& data)
    {
      return tailTestApeFooterOffset(data) - tailTestApeTagSize(data);
    }

    std::vector<TailTestLyricsField> tailTestDefaultLyricsFields()
    {
      return {{.id = "LYR", .value = "Trailing tag lyrics text"}, {.id = "INF", .value = "Trailing tag info"}};
    }

    void appendTailTestLyrics3Tag(std::vector<std::uint8_t>& data, std::vector<TailTestLyricsField> const& fields)
    {
      auto const tagOffset = data.size();
      appendTailTestAscii(data, "LYRICSBEGIN");

      for (TailTestLyricsField const& field : fields)
      {
        appendTailTestAscii(data, field.id);
        appendTailTestDecimalDigits(data, field.value.size(), kTailTestLyricsSizeDigits);
        appendTailTestAscii(data, field.value);
      }

      appendTailTestDecimalDigits(data, data.size() - tagOffset, kTailTestLyricsLengthDigits);
      appendTailTestAscii(data, "LYRICS200");
    }

    TailTestSessions openTailTestSessions(std::filesystem::path const& path,
                                          std::optional<SampleEncoding> const optEncoding)
    {
      return TailTestSessions{.concretePtr = ao::test::requireValue(Mp3DecoderSession::open(path, optEncoding)),
                              .factoryPtr = ao::test::requireValue(openDecoderSession(path, optEncoding))};
    }

    void checkTailTestIdentified(DecodedStreamInfo const& info,
                                 TailTestModeCase const& expected,
                                 SampleEncoding const encoding)
    {
      CHECK(info.codec == AudioCodec::Mp3);
      CHECK(info.isLossy);
      CHECK(info.sourceFormat == SignalFormat{.sampleRate = kTailTestSampleRate,
                                              .channels = expected.channels,
                                              .precisionBits = 16,
                                              .sampleKind = SampleKind::Integer,
                                              .speakerMask = expected.speakerMask});
      CHECK(info.outputFormat == PcmFormat{.sampleRate = kTailTestSampleRate,
                                           .channels = expected.channels,
                                           .encoding = encoding,
                                           .speakerMask = expected.speakerMask});
      CHECK(info.sourceChannelLayout.origin == ChannelLayoutOrigin::Identified);
      CHECK_FALSE(info.optTotalFrames);
    }

    void checkTailTestUnknown(DecodedStreamInfo const& info, std::uint8_t const channels, SampleEncoding const encoding)
    {
      CHECK(info.codec == AudioCodec::Mp3);
      CHECK(info.isLossy);
      CHECK(info.sourceFormat == SignalFormat{.sampleRate = kTailTestSampleRate,
                                              .channels = channels,
                                              .precisionBits = 16,
                                              .sampleKind = SampleKind::Integer,
                                              .speakerMask = 0});
      CHECK(info.outputFormat ==
            PcmFormat{.sampleRate = kTailTestSampleRate, .channels = channels, .encoding = encoding, .speakerMask = 0});
      CHECK(info.sourceChannelLayout.origin == ChannelLayoutOrigin::Unknown);
      CHECK_FALSE(info.optTotalFrames);
    }

    void checkTailTestSilentBlock(PcmBlock const& block,
                                  std::uint64_t const expectedFirstFrameIndex,
                                  std::uint8_t const channels,
                                  std::size_t const sampleBytes)
    {
      REQUIRE(block.frames > 0);
      CHECK(block.firstFrameIndex == expectedFirstFrameIndex);
      REQUIRE(block.bytes.size() == static_cast<std::size_t>(block.frames) * channels * sampleBytes);
      CHECK(std::ranges::all_of(block.bytes, [](std::byte const val) { return val == std::byte{0}; }));
    }

    void expectTailTestStableEos(DecoderSession& decoder, DecodedStreamInfo const& expectedInfo)
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

    void decodeTailTestSilentRange(DecoderSession& decoder,
                                   std::uint64_t const firstFrameIndex,
                                   std::uint64_t const endFrameIndex,
                                   DecodedStreamInfo const& expectedInfo)
    {
      auto const channels = expectedInfo.outputFormat.channels;
      auto const sampleBytes = bytesPerSample(expectedInfo.outputFormat.encoding);
      auto totalFrames = firstFrameIndex;

      while (totalFrames < endFrameIndex)
      {
        auto const blockRes = decoder.readNextBlock();
        REQUIRE(blockRes);
        REQUIRE_FALSE(blockRes->endOfStream);
        REQUIRE(totalFrames + blockRes->frames <= endFrameIndex);
        checkTailTestSilentBlock(*blockRes, totalFrames, channels, sampleBytes);
        totalFrames += blockRes->frames;
        CHECK(decoder.streamInfo() == expectedInfo);
      }

      CHECK(totalFrames == endFrameIndex);
    }

    void decodeTailTestSilentStream(DecoderSession& decoder, std::uint64_t const expectedTotalFrames)
    {
      auto const info = decoder.streamInfo();
      decodeTailTestSilentRange(decoder, 0, expectedTotalFrames, info);
      expectTailTestStableEos(decoder, info);
    }

    // Delivers every remaining frame without asserting sample values: a
    // candidate tag inside the final frame's payload overlaps bytes that the
    // frame still decodes, so silence is not part of that contract.
    void decodeTailTestContiguousFrames(DecoderSession& decoder, std::uint64_t const expectedTotalFrames)
    {
      auto const info = decoder.streamInfo();
      std::uint64_t totalFrames = 0;

      while (totalFrames < expectedTotalFrames)
      {
        auto const blockRes = decoder.readNextBlock();
        REQUIRE(blockRes);
        REQUIRE_FALSE(blockRes->endOfStream);
        REQUIRE(totalFrames + blockRes->frames <= expectedTotalFrames);
        CHECK(blockRes->firstFrameIndex == totalFrames);
        totalFrames += blockRes->frames;
      }

      CHECK(totalFrames == expectedTotalFrames);
      CHECK(decoder.streamInfo() == info);
    }

    // An invalid recognized suffix keeps the whole original input, and the
    // contract leaves the terminal behavior of the unrecognized tail open.
    // Accept a clean end of stream or a terminal read error, but never a
    // further identity change after the intact prefix.
    void tolerateTailTestUnrecognizedEnding(DecoderSession& decoder, DecodedStreamInfo const& expectedInfo)
    {
      for (std::uint32_t attempt = 0; attempt < 16U; ++attempt)
      {
        auto const blockRes = decoder.readNextBlock();

        if (!blockRes)
        {
          break;
        }

        if (blockRes->endOfStream)
        {
          CHECK(blockRes->frames == 0);
          CHECK(blockRes->bytes.empty());
          break;
        }
      }

      CHECK(decoder.streamInfo() == expectedInfo);
    }

    std::vector<std::byte> collectTailTestPcmBytes(DecoderSession& decoder, std::uint64_t nextFrameIndex = 0)
    {
      auto pcm = std::vector<std::byte>{};

      while (true)
      {
        auto const blockRes = decoder.readNextBlock();
        REQUIRE(blockRes);

        if (blockRes->endOfStream)
        {
          CHECK(blockRes->frames == 0);
          CHECK(blockRes->bytes.empty());
          break;
        }

        CHECK(blockRes->firstFrameIndex == nextFrameIndex);
        pcm.insert(pcm.end(), blockRes->bytes.begin(), blockRes->bytes.end());
        nextFrameIndex += blockRes->frames;
      }

      auto const repeatRes = decoder.readNextBlock();
      REQUIRE(repeatRes);
      CHECK(repeatRes->endOfStream);

      return pcm;
    }
  } // namespace

  TEST_CASE("Mp3DecoderSession - recognized APEv2 footer tail keeps mono and stereo identity to a clean end of stream")
  {
    auto const modeCases = std::vector{kTailTestStereoMode, kTailTestMonoMode};

    for (TailTestModeCase const& modeCase : modeCases)
    {
      CAPTURE(static_cast<int>(modeCase.mode));
      auto data = buildTailTestFrameRun(modeCase.mode, 5);
      appendTailTestApeTag(data, tailTestDefaultApeItems(), TailTestApeLayout::FooterOnly);
      auto const temp = ao::test::TempFile{data, ".mp3"};
      auto opened = openTailTestSessions(temp.path, std::nullopt);
      auto const info = opened.concretePtr->streamInfo();
      checkTailTestIdentified(info, modeCase, SampleEncoding::Signed16Le);

      // Five fixture frames decode to 5760 PCM frames; the scanned length
      // floors to 130 ms at 44100 Hz, exactly as without the tag.
      CHECK(info.duration == std::chrono::milliseconds{130});
      CHECK(opened.factoryPtr->streamInfo() == info);
      decodeTailTestSilentStream(*opened.concretePtr, kTailTestFiveFramePcmFrames);
      decodeTailTestSilentStream(*opened.factoryPtr, kTailTestFiveFramePcmFrames);
      CHECK(opened.concretePtr->streamInfo() == info);
      CHECK(opened.factoryPtr->streamInfo() == info);
    }
  }

  TEST_CASE("Mp3DecoderSession - a matching APEv2 header and footer tail stays identified across seeks")
  {
    auto data = buildTailTestFrameRun(TailTestChannelMode::Stereo, 5);
    appendTailTestApeTag(data, tailTestDefaultApeItems(), TailTestApeLayout::HeaderAndFooter);
    auto const temp = ao::test::TempFile{data, ".mp3"};
    auto opened = openTailTestSessions(temp.path, std::nullopt);
    auto& decoder = *opened.concretePtr;
    auto const info = decoder.streamInfo();
    checkTailTestIdentified(info, kTailTestStereoMode, SampleEncoding::Signed16Le);
    CHECK(info.duration == std::chrono::milliseconds{130});
    CHECK(opened.factoryPtr->streamInfo() == info);

    decodeTailTestSilentStream(decoder, kTailTestFiveFramePcmFrames);

    REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
    decoder.flush();
    CHECK(decoder.streamInfo() == info);
    decodeTailTestSilentStream(decoder, kTailTestFiveFramePcmFrames);

    // 120 ms is sample 5292, inside the fifth frame (4608..5760).
    REQUIRE(decoder.seek(std::chrono::milliseconds{120}));
    CHECK(decoder.streamInfo() == info);
    decodeTailTestSilentRange(decoder, 5292, kTailTestFiveFramePcmFrames, info);
    expectTailTestStableEos(decoder, info);

    REQUIRE(decoder.seek(std::chrono::milliseconds{130}));
    decodeTailTestSilentRange(decoder, 5733, kTailTestFiveFramePcmFrames, info);
    expectTailTestStableEos(decoder, info);
    auto const beyondEndRes = decoder.seek(std::chrono::milliseconds{131});
    REQUIRE_FALSE(beyondEndRes);
    CHECK(beyondEndRes.error().code == Error::Code::SeekFailed);
    expectTailTestStableEos(decoder, info);

    REQUIRE(opened.factoryPtr->seek(std::chrono::milliseconds{120}));
    auto const factoryBlockRes = opened.factoryPtr->readNextBlock();
    REQUIRE(factoryBlockRes);
    checkTailTestSilentBlock(*factoryBlockRes, 5292, 2, bytesPerSample(SampleEncoding::Signed16Le));
    CHECK(opened.factoryPtr->streamInfo() == info);
  }

  TEST_CASE("Mp3DecoderSession - every supported trailing tag chain keeps stereo identity and the exact PCM extent")
  {
    using ChainAppender = void (*)(std::vector<std::uint8_t>&);
    auto const chains = std::vector<std::pair<char const*, ChainAppender>>{
      {"id3v1", [](std::vector<std::uint8_t>& data) { appendTailTestId3v1Tag(data); }},
      {"ape-footer", [](std::vector<std::uint8_t>& data) { appendTailTestApeTag(data, tailTestDefaultApeItems()); }},
      {"ape-header-footer",
       [](std::vector<std::uint8_t>& data)
       { appendTailTestApeTag(data, tailTestDefaultApeItems(), TailTestApeLayout::HeaderAndFooter); }},
      {"ape-footer-id3v1",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems());
         appendTailTestId3v1Tag(data);
       }},
      {"ape-header-footer-id3v1",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems(), TailTestApeLayout::HeaderAndFooter);
         appendTailTestId3v1Tag(data);
       }},
      {"lyrics3-id3v1",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestLyrics3Tag(data, tailTestDefaultLyricsFields());
         appendTailTestId3v1Tag(data);
       }},
      {"lyrics3-ape-footer-id3v1",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestLyrics3Tag(data, tailTestDefaultLyricsFields());
         appendTailTestApeTag(data, tailTestDefaultApeItems());
         appendTailTestId3v1Tag(data);
       }},
      {"lyrics3-ape-header-footer-id3v1",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestLyrics3Tag(data, tailTestDefaultLyricsFields());
         appendTailTestApeTag(data, tailTestDefaultApeItems(), TailTestApeLayout::HeaderAndFooter);
         appendTailTestId3v1Tag(data);
       }},
    };

    for (auto const& [chainName, appendChain] : chains)
    {
      CAPTURE(chainName);
      auto data = buildTailTestFrameRun(TailTestChannelMode::Stereo, 5);
      appendChain(data);
      auto const temp = ao::test::TempFile{data, ".mp3"};
      auto opened = openTailTestSessions(temp.path, std::nullopt);
      auto const info = opened.concretePtr->streamInfo();
      checkTailTestIdentified(info, kTailTestStereoMode, SampleEncoding::Signed16Le);
      CHECK(info.duration == std::chrono::milliseconds{130});
      CHECK(opened.factoryPtr->streamInfo() == info);
      decodeTailTestSilentStream(*opened.concretePtr, kTailTestFiveFramePcmFrames);
      CHECK(opened.concretePtr->streamInfo() == info);
    }
  }

  TEST_CASE(
    "Mp3DecoderSession - an exact 128-byte ID3v1 suffix keeps a false-Layer-I-prefixed 48 kHz stereo stream identified")
  {
    // The four leading bytes read as a false MPEG1 Layer I frame header whose
    // 576-byte extent reaches into the tag; only the five 96-byte
    // MPEG1 Layer III frames at 32 kbit/s and 48000 Hz are audio, and the
    // exact 128-byte TAG suffix is cropped after the native scan witnesses
    // the fifth frame ending at the tag boundary.
    auto data = std::vector<std::uint8_t>{0xFF, 0xFF, 0xC8, 0x00};
    auto const wideFrames = buildTailTestWideFrameRun(TailTestChannelMode::Stereo, 5);
    data.insert(data.end(), wideFrames.begin(), wideFrames.end());
    appendTailTestId3v1Tag(data);

    auto const temp = ao::test::TempFile{data, ".mp3"};
    auto opened = openTailTestSessions(temp.path, std::nullopt);
    auto& decoder = *opened.concretePtr;
    auto const info = decoder.streamInfo();

    // checkTailTestIdentified asserts the shared 44100 Hz fixture rate; this
    // 48000 Hz stream pins its geometry explicitly instead of weakening the
    // shared helper.
    CHECK(info.codec == AudioCodec::Mp3);
    CHECK(info.isLossy);
    CHECK(info.sourceFormat == SignalFormat{.sampleRate = kTailTestWideSampleRate,
                                            .channels = 2,
                                            .precisionBits = 16,
                                            .sampleKind = SampleKind::Integer,
                                            .speakerMask = 0x3});
    CHECK(info.outputFormat == PcmFormat{.sampleRate = kTailTestWideSampleRate,
                                         .channels = 2,
                                         .encoding = SampleEncoding::Signed16Le,
                                         .speakerMask = 0x3});
    CHECK(info.sourceChannelLayout.origin == ChannelLayoutOrigin::Identified);
    CHECK_FALSE(info.optTotalFrames);
    // Five 96-byte frames decode to 5760 PCM frames, exactly 120 ms at 48000 Hz.
    CHECK(info.duration == std::chrono::milliseconds{120});
    CHECK(opened.factoryPtr->streamInfo() == info);

    decodeTailTestSilentStream(decoder, kTailTestFiveFramePcmFrames);
    decodeTailTestSilentStream(*opened.factoryPtr, kTailTestFiveFramePcmFrames);
    CHECK(decoder.streamInfo() == info);
    CHECK(opened.factoryPtr->streamInfo() == info);

    REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
    decoder.flush();
    CHECK(decoder.streamInfo() == info);
    decodeTailTestSilentStream(decoder, kTailTestFiveFramePcmFrames);

    // 60 ms is sample 2880, inside the third frame (2304..3455).
    REQUIRE(decoder.seek(std::chrono::milliseconds{60}));
    CHECK(decoder.streamInfo() == info);
    decodeTailTestSilentRange(decoder, 2880, kTailTestFiveFramePcmFrames, info);
    expectTailTestStableEos(decoder, info);

    // 120 ms is sample 5760, the exact stream end.
    REQUIRE(decoder.seek(std::chrono::milliseconds{120}));
    expectTailTestStableEos(decoder, info);

    auto const beyondEndRes = decoder.seek(std::chrono::milliseconds{121});
    REQUIRE_FALSE(beyondEndRes);
    CHECK(beyondEndRes.error().code == Error::Code::SeekFailed);
    expectTailTestStableEos(decoder, info);
  }

  TEST_CASE("Mp3DecoderSession - fake frames and tag markers inside a valid ID3v1 body keep the plain ID3v1 contract")
  {
    auto const modeCases = std::vector{kTailTestStereoMode, kTailTestMonoMode};

    for (TailTestModeCase const& modeCase : modeCases)
    {
      CAPTURE(static_cast<int>(modeCase.mode));
      auto data = buildTailTestFrameRun(modeCase.mode, 5);

      // The valid ID3v1 body carries two fake 48-byte MPEG2.5 frames plus
      // TAG, APETAGEX, and ID3 markers: none of these may turn the tail into
      // anything but one plain 128-byte ID3v1 suffix. The remaining fifteen
      // body bytes stay zero.
      auto body = std::vector<std::uint8_t>{};

      for (std::size_t frameIndex = 0; frameIndex < 2; ++frameIndex)
      {
        auto const fakeFrame = buildTailTestFakeMpeg25Frame();
        body.insert(body.end(), fakeFrame.begin(), fakeFrame.end());
      }

      for (std::string_view const marker : {"TAG", "APETAGEX", "ID3"})
      {
        appendTailTestAscii(body, marker);
      }

      REQUIRE(body.size() == 110U);

      auto const tagOffset = data.size();
      appendTailTestId3v1Tag(data);
      std::ranges::copy(body, data.begin() + static_cast<std::ptrdiff_t>(tagOffset + 3U));

      auto const temp = ao::test::TempFile{data, ".mp3"};
      auto opened = openTailTestSessions(temp.path, std::nullopt);
      auto const info = opened.concretePtr->streamInfo();
      checkTailTestIdentified(info, modeCase, SampleEncoding::Signed16Le);
      CHECK(info.duration == std::chrono::milliseconds{130});
      CHECK(opened.factoryPtr->streamInfo() == info);
      decodeTailTestSilentStream(*opened.concretePtr, kTailTestFiveFramePcmFrames);
      decodeTailTestSilentStream(*opened.factoryPtr, kTailTestFiveFramePcmFrames);
      CHECK(opened.concretePtr->streamInfo() == info);
      CHECK(opened.factoryPtr->streamInfo() == info);
    }
  }

  TEST_CASE("Mp3DecoderSession - trailing APEv2 and Lyrics3v2 tags beyond one kilobyte end in a clean end of stream")
  {
    auto apeData = buildTailTestFrameRun(TailTestChannelMode::Stereo, 5);
    appendTailTestApeTag(apeData, {{.key = "Lyrics", .value = std::vector<std::uint8_t>(2000, 0xD7U), .flags = 2U}});

    auto lyricsData = buildTailTestFrameRun(TailTestChannelMode::Stereo, 5);
    appendTailTestLyrics3Tag(lyricsData, {{.id = "LYR", .value = std::string(2000, 'l')}});
    appendTailTestId3v1Tag(lyricsData);

    auto const streams = std::vector{apeData, lyricsData};

    for (std::size_t streamIndex = 0; streamIndex < streams.size(); ++streamIndex)
    {
      CAPTURE(streamIndex);
      auto const temp = ao::test::TempFile{streams[streamIndex], ".mp3"};
      auto opened = openTailTestSessions(temp.path, std::nullopt);
      auto const info = opened.concretePtr->streamInfo();
      checkTailTestIdentified(info, kTailTestStereoMode, SampleEncoding::Signed16Le);
      CHECK(info.duration == std::chrono::milliseconds{130});
      CHECK(opened.factoryPtr->streamInfo() == info);
      decodeTailTestSilentStream(*opened.concretePtr, kTailTestFiveFramePcmFrames);
      CHECK(opened.concretePtr->streamInfo() == info);
    }
  }

  TEST_CASE("Mp3DecoderSession - a binary APEv2 value carrying fake dual frames is excluded from decoding")
  {
    auto data = buildTailTestFrameRun(TailTestChannelMode::Stereo, 5);
    auto const fakeDualFrames = buildTailTestFrameRun(TailTestChannelMode::DualChannel, 3);
    appendTailTestApeTag(data, {{.key = "Cover", .value = fakeDualFrames, .flags = 2U}});
    auto const temp = ao::test::TempFile{data, ".mp3"};
    auto opened = openTailTestSessions(temp.path, std::nullopt);
    auto const info = opened.concretePtr->streamInfo();
    checkTailTestIdentified(info, kTailTestStereoMode, SampleEncoding::Signed16Le);
    CHECK(info.duration == std::chrono::milliseconds{130});
    CHECK(opened.factoryPtr->streamInfo() == info);

    // Exactly five MPEG frames decode: the three dual frames inside the
    // opaque item value never reach the native reader.
    decodeTailTestSilentStream(*opened.concretePtr, kTailTestFiveFramePcmFrames);
    CHECK(opened.concretePtr->streamInfo() == info);
  }

  TEST_CASE("Mp3DecoderSession - read-only APEv2 footer and item flags keep identity and the exact PCM extent")
  {
    // The APEv2 read-only bit is valid on footer flags alongside the
    // header-present bit, and items may be read-only themselves.
    auto const readOnlyItems =
      std::vector<TailTestApeItem>{{.key = "Artist", .value = tailTestAsciiBytes("Read-Only Artist"), .flags = 1U},
                                   {.key = "Cover", .value = std::vector<std::uint8_t>(96, 0xB7U), .flags = 3U}};

    for (auto const layout :
         std::to_array<TailTestApeLayout>({TailTestApeLayout::FooterOnly, TailTestApeLayout::HeaderAndFooter}))
    {
      for (bool const withId3v1 : std::to_array<bool>({false, true}))
      {
        CAPTURE(static_cast<int>(layout), withId3v1);
        auto data = buildTailTestFrameRun(TailTestChannelMode::Stereo, 5);
        appendTailTestApeTag(data, readOnlyItems, layout, kTailTestApeReadOnlyFlag);

        if (withId3v1)
        {
          appendTailTestId3v1Tag(data);
        }

        auto const temp = ao::test::TempFile{data, ".mp3"};
        auto opened = openTailTestSessions(temp.path, std::nullopt);
        auto const info = opened.concretePtr->streamInfo();
        checkTailTestIdentified(info, kTailTestStereoMode, SampleEncoding::Signed16Le);
        CHECK(info.duration == std::chrono::milliseconds{130});
        CHECK(opened.factoryPtr->streamInfo() == info);
        decodeTailTestSilentStream(*opened.concretePtr, kTailTestFiveFramePcmFrames);
        CHECK(opened.concretePtr->streamInfo() == info);
      }
    }
  }

  TEST_CASE("Mp3DecoderSession - an exactly 128-byte APEv2-only tail is not mistaken for an ID3v1 suffix")
  {
    // Eight item-framing bytes, "Title" plus its NUL terminator, an 82-byte
    // binary value beginning with a fake 48-byte MPEG2.5 frame, and the
    // 32-byte footer sum to exactly 128 tag bytes. The reuse path is
    // reserved for a genuine TAG-only suffix, so this tail still rescans
    // before any identity is published.
    auto value = buildTailTestFakeMpeg25Frame();
    value.insert(value.end(), 82U - kTailTestFakeMpeg25FrameBytes, 0x55U);

    auto data = buildTailTestFrameRun(TailTestChannelMode::Stereo, 5);
    appendTailTestApeTag(data, {{.key = "Title", .value = value, .flags = 2U}});
    REQUIRE(data.size() - (5U * kTailTestMpegFrameBytes) == 128U);

    auto const temp = ao::test::TempFile{data, ".mp3"};
    auto opened = openTailTestSessions(temp.path, std::nullopt);
    auto const info = opened.concretePtr->streamInfo();
    checkTailTestIdentified(info, kTailTestStereoMode, SampleEncoding::Signed16Le);
    CHECK(info.duration == std::chrono::milliseconds{130});
    CHECK(opened.factoryPtr->streamInfo() == info);

    // Exactly five real MPEG frames decode: the fake frame inside the cropped
    // tag value never reaches the native reader.
    decodeTailTestSilentStream(*opened.concretePtr, kTailTestFiveFramePcmFrames);
    CHECK(opened.concretePtr->streamInfo() == info);
  }

  TEST_CASE("Mp3DecoderSession - malformed APEv2 tails force unknown identity while the intact prefix stays open")
  {
    using TailAppender = void (*)(std::vector<std::uint8_t>&);
    auto const cases = std::vector<std::pair<char const*, TailAppender>>{
      {"footer version is not 2000",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems());
         overwriteTailTestU32Le(
           data, tailTestApeFooterOffset(data) + kTailTestApeVersionOffset, kTailTestApeVersion2000 - 1U);
       }},
      {"footer flags use an unsupported bit",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems());
         overwriteTailTestU32Le(data, tailTestApeFooterOffset(data) + kTailTestApeFlagsOffset, 0x40000000U);
       }},
      {"footer reserved bytes are nonzero",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems());
         data[tailTestApeFooterOffset(data) + kTailTestApeReservedOffset] = 1U;
       }},
      {"tag size disagrees with the items plus footer",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems());
         auto const footOffset = tailTestApeFooterOffset(data);
         overwriteTailTestU32Le(data,
                                footOffset + kTailTestApeSizeOffset,
                                readTailTestU32Le(data, footOffset + kTailTestApeSizeOffset) +
                                  static_cast<std::uint32_t>(kTailTestApeTagPartBytes));
       }},
      {"tag size exceeds the file and the recognized tail cap",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems());
         // 33 MiB: beyond the 32 MiB recognized-tail cap and far beyond the file.
         overwriteTailTestU32Le(data, tailTestApeFooterOffset(data) + kTailTestApeSizeOffset, 0x02100000U);
       }},
      {"item count disagrees with the present items",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems());
         overwriteTailTestU32Le(data, tailTestApeFooterOffset(data) + kTailTestApeCountOffset, 3U);
       }},
      {"item flags use the reserved value type",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems());
         overwriteTailTestU32Le(data, tailTestApeItemsOffset(data) + 4U, 6U);
       }},
      {"item flags use a bit beyond the type and read-only bits",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems());
         overwriteTailTestU32Le(data, tailTestApeItemsOffset(data) + 4U, 0x00000008U);
       }},
      {"item key is never NUL terminated",
       [](std::vector<std::uint8_t>& data)
       {
         auto const unterminatedKeyItem =
           TailTestApeItem{.key = "Title", .value = std::vector<std::uint8_t>(300, 0xAAU)};
         appendTailTestApeTag(data, {unterminatedKeyItem});
         auto const keyStart = tailTestApeItemsOffset(data) + 8U;
         auto const terminator = std::find(data.begin() + static_cast<std::ptrdiff_t>(keyStart), data.end(), 0U);
         REQUIRE(terminator != data.end());
         *terminator = static_cast<std::uint8_t>('K');
       }},
      {"item value length overruns the tag",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems());
         auto const itemsOffset = tailTestApeItemsOffset(data);
         overwriteTailTestU32Le(data, itemsOffset, readTailTestU32Le(data, itemsOffset) + 64U);
       }},
      {"footer announces a header without the magic",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems(), TailTestApeLayout::HeaderAndFooter);
         data[tailTestApeHeaderOffset(data) + 7U] = static_cast<std::uint8_t>('Y');
       }},
      {"header size disagrees with the footer",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems(), TailTestApeLayout::HeaderAndFooter);
         auto const headerOffset = tailTestApeHeaderOffset(data);
         overwriteTailTestU32Le(data,
                                headerOffset + kTailTestApeSizeOffset,
                                readTailTestU32Le(data, headerOffset + kTailTestApeSizeOffset) + 1U);
       }},
      {"header flags are not the header flags",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems(), TailTestApeLayout::HeaderAndFooter);
         overwriteTailTestU32Le(
           data, tailTestApeHeaderOffset(data) + kTailTestApeFlagsOffset, kTailTestApeFooterHasHeaderFlag);
       }},
    };

    for (auto const& [caseName, appendMalformedTail] : cases)
    {
      CAPTURE(caseName);
      auto data = buildTailTestFrameRun(TailTestChannelMode::Stereo, 5);
      appendMalformedTail(data);
      auto const temp = ao::test::TempFile{data, ".mp3"};
      auto opened = openTailTestSessions(temp.path, std::nullopt);
      auto const info = opened.concretePtr->streamInfo();
      checkTailTestUnknown(info, 2, SampleEncoding::Signed16Le);
      CHECK(opened.factoryPtr->streamInfo() == info);
      decodeTailTestSilentRange(*opened.concretePtr, 0, kTailTestFiveFramePcmFrames, info);
      tolerateTailTestUnrecognizedEnding(*opened.concretePtr, info);
      CHECK(opened.factoryPtr->streamInfo() == info);
    }
  }

  TEST_CASE(
    "Mp3DecoderSession - read-only disagreement between an APEv2 header and footer stays transactionally unknown")
  {
    using TailAppender = void (*)(std::vector<std::uint8_t>&);
    auto const cases = std::vector<std::pair<char const*, TailAppender>>{
      {"footer is read-only but the header is not",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(
           data, tailTestDefaultApeItems(), TailTestApeLayout::HeaderAndFooter, kTailTestApeReadOnlyFlag);
         overwriteTailTestU32Le(data,
                                tailTestApeHeaderOffset(data) + kTailTestApeFlagsOffset,
                                kTailTestApeFooterHasHeaderFlag | kTailTestApeIsHeaderFlag);
       }},
      {"header is read-only but the footer is not",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems(), TailTestApeLayout::HeaderAndFooter);
         overwriteTailTestU32Le(data,
                                tailTestApeHeaderOffset(data) + kTailTestApeFlagsOffset,
                                kTailTestApeFooterHasHeaderFlag | kTailTestApeIsHeaderFlag | kTailTestApeReadOnlyFlag);
       }},
    };

    for (auto const& [caseName, appendDisagreeingTail] : cases)
    {
      for (bool const withOuterId3v1 : std::to_array<bool>({false, true}))
      {
        CAPTURE(caseName, withOuterId3v1);
        auto data = buildTailTestFrameRun(TailTestChannelMode::Stereo, 5);
        appendDisagreeingTail(data);

        if (withOuterId3v1)
        {
          appendTailTestId3v1Tag(data);
        }

        auto const temp = ao::test::TempFile{data, ".mp3"};
        auto opened = openTailTestSessions(temp.path, std::nullopt);
        auto const info = opened.concretePtr->streamInfo();
        // The optional header must carry all footer flags plus the is-header
        // bit: a read-only disagreement is invalid, and a trailing ID3v1
        // cannot partially repair it.
        checkTailTestUnknown(info, 2, SampleEncoding::Signed16Le);
        CHECK(opened.factoryPtr->streamInfo() == info);
        decodeTailTestSilentRange(*opened.concretePtr, 0, kTailTestFiveFramePcmFrames, info);
        tolerateTailTestUnrecognizedEnding(*opened.concretePtr, info);
        CHECK(opened.concretePtr->streamInfo() == info);
      }
    }
  }

  TEST_CASE("Mp3DecoderSession - an APEv1 footer tail stays unsupported while the intact prefix still plays")
  {
    // APEv1 (version 1000) is a distinct unsupported format, not a malformed
    // APEv2 tag: identity stays unknown, the intact prefix still decodes, and
    // the terminal behavior after the prefix remains an open contract.
    auto data = buildTailTestFrameRun(TailTestChannelMode::Stereo, 5);
    appendTailTestApeTag(data, {{.key = "Artist", .value = tailTestAsciiBytes("Apev1 Artist")}});
    overwriteTailTestU32Le(data, tailTestApeFooterOffset(data) + kTailTestApeVersionOffset, kTailTestApeVersion1000);

    auto const temp = ao::test::TempFile{data, ".mp3"};
    auto opened = openTailTestSessions(temp.path, std::nullopt);
    auto const info = opened.concretePtr->streamInfo();
    checkTailTestUnknown(info, 2, SampleEncoding::Signed16Le);
    CHECK(opened.factoryPtr->streamInfo() == info);
    decodeTailTestSilentRange(*opened.concretePtr, 0, kTailTestFiveFramePcmFrames, info);
    tolerateTailTestUnrecognizedEnding(*opened.concretePtr, info);
    CHECK(opened.concretePtr->streamInfo() == info);
  }

  TEST_CASE("Mp3DecoderSession - malformed Lyrics3v2 tails force unknown identity while the intact prefix stays open")
  {
    using TailAppender = void (*)(std::vector<std::uint8_t>&);
    auto const cases = std::vector<std::pair<char const*, TailAppender>>{
      {"field size digits are not decimal",
       [](std::vector<std::uint8_t>& data)
       {
         auto const tagOffset = data.size();
         appendTailTestLyrics3Tag(data, tailTestDefaultLyricsFields());
         appendTailTestId3v1Tag(data);
         data[tagOffset + kTailTestLyricsBeginBytes + 3U] = static_cast<std::uint8_t>('x');
       }},
      {"field size overruns the declared fields",
       [](std::vector<std::uint8_t>& data)
       {
         auto const tagOffset = data.size();
         appendTailTestLyrics3Tag(data, tailTestDefaultLyricsFields());
         appendTailTestId3v1Tag(data);
         overwriteTailTestDecimalDigits(
           data, tagOffset + kTailTestLyricsBeginBytes + 3U, 99, kTailTestLyricsSizeDigits);
       }},
      {"total length does not match the tag",
       [](std::vector<std::uint8_t>& data)
       {
         auto const tagOffset = data.size();
         appendTailTestLyrics3Tag(data, tailTestDefaultLyricsFields());
         appendTailTestId3v1Tag(data);
         auto const lengthOffset =
           data.size() - kTailTestId3v1Bytes - kTailTestLyricsEndBytes - kTailTestLyricsLengthDigits;
         overwriteTailTestDecimalDigits(
           data, lengthOffset, data.size() - kTailTestId3v1Bytes - tagOffset + 1U, kTailTestLyricsLengthDigits);
       }},
      {"total length contains a non-digit",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestLyrics3Tag(data, tailTestDefaultLyricsFields());
         appendTailTestId3v1Tag(data);
         auto const lengthOffset =
           data.size() - kTailTestId3v1Bytes - kTailTestLyricsEndBytes - kTailTestLyricsLengthDigits;
         data[lengthOffset + 2U] = static_cast<std::uint8_t>(' ');
       }},
      {"field id is not three uppercase characters",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestLyrics3Tag(data, {{.id = "lyr", .value = "lowercase field id"}});
         appendTailTestId3v1Tag(data);
       }},
      {"tail without a trailing id3v1",
       [](std::vector<std::uint8_t>& data) { appendTailTestLyrics3Tag(data, tailTestDefaultLyricsFields()); }},
      {"tail without any field",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestLyrics3Tag(data, {});
         appendTailTestId3v1Tag(data);
       }},
      {"tail before an ape tag without id3v1",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestLyrics3Tag(data, tailTestDefaultLyricsFields());
         appendTailTestApeTag(data, tailTestDefaultApeItems());
       }},
    };

    for (auto const& [caseName, appendMalformedTail] : cases)
    {
      CAPTURE(caseName);
      auto data = buildTailTestFrameRun(TailTestChannelMode::Stereo, 5);
      appendMalformedTail(data);
      auto const temp = ao::test::TempFile{data, ".mp3"};
      auto opened = openTailTestSessions(temp.path, std::nullopt);
      auto const info = opened.concretePtr->streamInfo();
      checkTailTestUnknown(info, 2, SampleEncoding::Signed16Le);
      CHECK(opened.factoryPtr->streamInfo() == info);
      decodeTailTestSilentRange(*opened.concretePtr, 0, kTailTestFiveFramePcmFrames, info);
      tolerateTailTestUnrecognizedEnding(*opened.concretePtr, info);
      CHECK(opened.factoryPtr->streamInfo() == info);
    }
  }

  TEST_CASE("Mp3DecoderSession - a malformed tail before a valid ID3v1 tag still forces unknown identity")
  {
    using TailAppender = void (*)(std::vector<std::uint8_t>&);
    auto const cases = std::vector<std::pair<char const*, TailAppender>>{
      {"ape tag size mismatch",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems());
         auto const footOffset = tailTestApeFooterOffset(data);
         overwriteTailTestU32Le(data,
                                footOffset + kTailTestApeSizeOffset,
                                readTailTestU32Le(data, footOffset + kTailTestApeSizeOffset) +
                                  static_cast<std::uint32_t>(kTailTestApeTagPartBytes));
         appendTailTestId3v1Tag(data);
       }},
      {"ape header mismatch",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems(), TailTestApeLayout::HeaderAndFooter);
         data[tailTestApeHeaderOffset(data) + 7U] = static_cast<std::uint8_t>('Y');
         appendTailTestId3v1Tag(data);
       }},
      {"lyrics total length mismatch",
       [](std::vector<std::uint8_t>& data)
       {
         auto const tagOffset = data.size();
         appendTailTestLyrics3Tag(data, tailTestDefaultLyricsFields());
         auto const lengthOffset = data.size() - kTailTestLyricsEndBytes - kTailTestLyricsLengthDigits;
         overwriteTailTestDecimalDigits(data, lengthOffset, data.size() - tagOffset + 1U, kTailTestLyricsLengthDigits);
         appendTailTestId3v1Tag(data);
       }},
    };

    for (auto const& [caseName, appendMalformedTail] : cases)
    {
      CAPTURE(caseName);
      auto data = buildTailTestFrameRun(TailTestChannelMode::Stereo, 5);
      appendMalformedTail(data);
      auto const temp = ao::test::TempFile{data, ".mp3"};
      auto opened = openTailTestSessions(temp.path, std::nullopt);
      auto const info = opened.concretePtr->streamInfo();
      checkTailTestUnknown(info, 2, SampleEncoding::Signed16Le);
      CHECK(opened.factoryPtr->streamInfo() == info);
      decodeTailTestSilentRange(*opened.concretePtr, 0, kTailTestFiveFramePcmFrames, info);
      tolerateTailTestUnrecognizedEnding(*opened.concretePtr, info);
      CHECK(opened.factoryPtr->streamInfo() == info);
    }
  }

  TEST_CASE("Mp3DecoderSession - repeated or misordered recognized tails are transactionally unknown")
  {
    using TailAppender = void (*)(std::vector<std::uint8_t>&);
    auto const cases = std::vector<std::pair<char const*, TailAppender>>{
      {"two ape footers",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems());
         appendTailTestApeTag(data, tailTestDefaultApeItems());
       }},
      {"id3v1 before an ape footer",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestId3v1Tag(data);
         appendTailTestApeTag(data, tailTestDefaultApeItems());
       }},
      {"ape footer before lyrics3 before id3v1",
       [](std::vector<std::uint8_t>& data)
       {
         appendTailTestApeTag(data, tailTestDefaultApeItems());
         appendTailTestLyrics3Tag(data, tailTestDefaultLyricsFields());
         appendTailTestId3v1Tag(data);
       }},
    };

    for (auto const& [caseName, appendUnsupportedTail] : cases)
    {
      CAPTURE(caseName);
      auto data = buildTailTestFrameRun(TailTestChannelMode::Stereo, 5);
      appendUnsupportedTail(data);
      auto const temp = ao::test::TempFile{data, ".mp3"};
      auto opened = openTailTestSessions(temp.path, std::nullopt);
      auto const info = opened.concretePtr->streamInfo();
      checkTailTestUnknown(info, 2, SampleEncoding::Signed16Le);
      CHECK(opened.factoryPtr->streamInfo() == info);
      decodeTailTestSilentRange(*opened.concretePtr, 0, kTailTestFiveFramePcmFrames, info);
      tolerateTailTestUnrecognizedEnding(*opened.concretePtr, info);
      CHECK(opened.factoryPtr->streamInfo() == info);
    }
  }

  TEST_CASE("Mp3DecoderSession - a tag candidate inside the final frame payload keeps all PCM and stays unknown")
  {
    // A "TAG" candidate sits at byte EOF-128 inside the fifth frame's
    // payload, with no appended tag bytes: the candidate start is not the end
    // of a complete indexed frame, so nothing may be cropped.
    auto tagInsideData = buildTailTestFrameRun(TailTestChannelMode::Stereo, 5);
    auto const tagOffset = tagInsideData.size() - kTailTestId3v1Bytes;
    tagInsideData[tagOffset] = static_cast<std::uint8_t>('T');
    tagInsideData[tagOffset + 1] = static_cast<std::uint8_t>('A');
    tagInsideData[tagOffset + 2] = static_cast<std::uint8_t>('G');

    // The analog control: a structurally valid footer-only APEv2 tag ends
    // exactly at EOF, fully inside the fifth frame's payload.
    auto apeCandidate = std::vector<std::uint8_t>{};
    appendTailTestApeTag(apeCandidate, {{.key = "Ti", .value = std::vector<std::uint8_t>(4, 0xB7U), .flags = 2U}});
    REQUIRE(apeCandidate.size() < kTailTestMpegFrameBytes);
    auto apeInsideData = buildTailTestFrameRun(TailTestChannelMode::Stereo, 5);
    std::ranges::copy(apeCandidate, apeInsideData.end() - static_cast<std::ptrdiff_t>(apeCandidate.size()));

    auto const streams = std::vector{tagInsideData, apeInsideData};

    for (std::size_t streamIndex = 0; streamIndex < streams.size(); ++streamIndex)
    {
      CAPTURE(streamIndex);
      auto const temp = ao::test::TempFile{streams[streamIndex], ".mp3"};
      auto opened = openTailTestSessions(temp.path, std::nullopt);
      auto const info = opened.concretePtr->streamInfo();
      checkTailTestUnknown(info, 2, SampleEncoding::Signed16Le);
      CHECK(opened.factoryPtr->streamInfo() == info);

      // The retained input loses neither the final frame's 1152 PCM frames
      // nor the original 130 ms duration.
      CHECK(info.duration == std::chrono::milliseconds{130});
      decodeTailTestContiguousFrames(*opened.concretePtr, kTailTestFiveFramePcmFrames);
      tolerateTailTestUnrecognizedEnding(*opened.concretePtr, info);
      CHECK(opened.concretePtr->streamInfo() == info);
    }
  }

  TEST_CASE("Mp3DecoderSession - recognized trailing tags do not hide an audio gap, a truncated frame, or late dual")
  {
    // A garbage gap between uniform stereo frames keeps the identity unknown.
    auto gapData = buildTailTestFrameRun(TailTestChannelMode::Stereo, 3);
    gapData.insert(gapData.end(), 32, 0x11U);
    auto const gapTrailingFrames = buildTailTestFrameRun(TailTestChannelMode::Stereo, 3);
    gapData.insert(gapData.end(), gapTrailingFrames.begin(), gapTrailingFrames.end());
    appendTailTestApeTag(gapData, tailTestDefaultApeItems(), TailTestApeLayout::HeaderAndFooter);
    appendTailTestId3v1Tag(gapData);

    // A truncated MPEG frame before the recognized chain stays unhidden.
    auto truncatedData = buildTailTestFrameRun(TailTestChannelMode::Stereo, 5);
    auto const partialFrame = buildTailTestMpegFrame(TailTestChannelMode::Stereo);
    truncatedData.insert(truncatedData.end(), partialFrame.begin(), partialFrame.begin() + 20);
    auto const audioAndPartialBytes = truncatedData.size();
    appendTailTestLyrics3Tag(truncatedData, tailTestDefaultLyricsFields());
    appendTailTestId3v1Tag(truncatedData);
    // The appended chain must stay too small to complete the partial frame:
    // 20 partial bytes plus the 83 + 128 tag-chain bytes stay below 417.
    REQUIRE(truncatedData.size() - audioAndPartialBytes + 20U < kTailTestMpegFrameBytes);

    // A dual frame past the one-thousand-frame probe keeps the identity
    // unknown even though the trailing APEv2 tag itself is valid.
    auto lateDualData = buildTailTestFrameRun(TailTestChannelMode::Stereo, kTailTestLateDualFrameCount - 1);
    auto const dualFrame = buildTailTestMpegFrame(TailTestChannelMode::DualChannel);
    lateDualData.insert(lateDualData.end(), dualFrame.begin(), dualFrame.end());
    appendTailTestApeTag(lateDualData, tailTestDefaultApeItems());

    auto const streams = std::vector<std::pair<std::vector<std::uint8_t>, std::uint64_t>>{
      {gapData, 6U * kTailTestPcmFramesPerMpegFrame},
      {truncatedData, kTailTestFiveFramePcmFrames},
      {lateDualData, kTailTestLateDualPcmFrames},
    };

    for (std::size_t streamIndex = 0; streamIndex < streams.size(); ++streamIndex)
    {
      CAPTURE(streamIndex);
      auto const& [data, pcmFrames] = streams[streamIndex];
      auto const temp = ao::test::TempFile{data, ".mp3"};
      auto opened = openTailTestSessions(temp.path, std::nullopt);
      auto const info = opened.concretePtr->streamInfo();
      checkTailTestUnknown(info, 2, SampleEncoding::Signed16Le);
      CHECK(opened.factoryPtr->streamInfo() == info);

      if (streamIndex == 2)
      {
        // 1001 Layer III frames floor to 26148 ms at 44100 Hz.
        CHECK(info.duration == std::chrono::milliseconds{26148});
      }

      if (streamIndex == 1)
      {
        // No frame-boundary witness exists before these tags: retain the original
        // partial input, including its native terminal error or EOF behavior.
        decodeTailTestSilentRange(*opened.concretePtr, 0, pcmFrames, info);
        tolerateTailTestUnrecognizedEnding(*opened.concretePtr, info);
      }
      else
      {
        decodeTailTestSilentStream(*opened.concretePtr, pcmFrames);
      }

      CHECK(opened.concretePtr->streamInfo() == info);
    }
  }

  TEST_CASE("Mp3DecoderSession - trailing tag capacity includes the complete outer chain")
  {
    constexpr std::size_t kTailCap = std::size_t{32} * 1024U * 1024U;
    // One item: eight length/flag bytes, "Ti" plus NUL, then the 32-byte footer.
    auto const items =
      std::vector{TailTestApeItem{.key = "Ti", .value = std::vector<std::uint8_t>(kTailCap - 43U, 0x55U), .flags = 2U}};

    for (bool const withId3v1 : std::to_array<bool>({false, true}))
    {
      CAPTURE(withId3v1);
      auto data = buildTailTestFrameRun(TailTestChannelMode::Stereo, 5);
      appendTailTestApeTag(data, items);

      if (withId3v1)
      {
        appendTailTestId3v1Tag(data);
      }

      auto const temp = ao::test::TempFile{data, ".mp3"};
      auto decoderPtr = ao::test::requireValue(Mp3DecoderSession::open(temp.path, std::nullopt));
      auto const info = decoderPtr->streamInfo();

      if (withId3v1)
      {
        checkTailTestUnknown(info, 2, SampleEncoding::Signed16Le);
        decodeTailTestSilentRange(*decoderPtr, 0, kTailTestFiveFramePcmFrames, info);
        // Oversized framing retains the original input, including the native resync failure.
        auto const tailRes = decoderPtr->readNextBlock();
        REQUIRE_FALSE(tailRes);
        CHECK(tailRes.error().code == Error::Code::DecodeFailed);
      }
      else
      {
        checkTailTestIdentified(info, kTailTestStereoMode, SampleEncoding::Signed16Le);
        decodeTailTestSilentStream(*decoderPtr, kTailTestFiveFramePcmFrames);
      }

      CHECK(decoderPtr->streamInfo() == info);
    }
  }

  TEST_CASE("Mp3DecoderSession - a tagged basic_metadata fixture decodes byte-identical PCM to the original")
  {
    auto const originalPath = requireAudioFixture("basic_metadata.mp3");
    auto openedOriginal = openTailTestSessions(originalPath, std::nullopt);
    auto const originalInfo = openedOriginal.concretePtr->streamInfo();
    CHECK(originalInfo.sourceChannelLayout.origin == ChannelLayoutOrigin::Identified);
    CHECK(originalInfo.sourceFormat.speakerMask == 0x3);
    auto const originalPcm = collectTailTestPcmBytes(*openedOriginal.concretePtr);
    REQUIRE_FALSE(originalPcm.empty());
    // The control protects real nonzero PCM, not only synthetic silence.
    CHECK(std::ranges::any_of(originalPcm, [](std::byte const val) { return val != std::byte{0}; }));

    auto taggedBytes = tailTestAsciiBytes(ao::test::readFile(originalPath));
    appendTailTestApeTag(taggedBytes, tailTestDefaultApeItems());
    auto const temp = ao::test::TempFile{taggedBytes, ".mp3"};
    auto openedTagged = openTailTestSessions(temp.path, std::nullopt);
    auto const taggedInfo = openedTagged.concretePtr->streamInfo();
    CHECK(taggedInfo == originalInfo);
    CHECK(openedTagged.factoryPtr->streamInfo() == taggedInfo);
    auto const taggedPcm = collectTailTestPcmBytes(*openedTagged.concretePtr);
    CHECK(taggedPcm.size() == originalPcm.size());
    CHECK(std::ranges::equal(taggedPcm, originalPcm));
  }

  TEST_CASE(
    "Mp3DecoderSession - ID3v1-tagged encoded fixtures decode byte-identical PCM and an equivalent seek history")
  {
    auto const fixtures = std::vector<std::pair<std::string_view, std::uint64_t>>{
      {"basic_metadata.mp3", 4410}, {"hires.mp3", 4800}, {"vbr_no_seek_table.mp3", 4410}};

    for (auto const& [fixtureName, firstFrameIndex] : fixtures)
    {
      CAPTURE(fixtureName);
      auto const originalPath = requireAudioFixture(fixtureName);
      auto openedOriginal = openTailTestSessions(originalPath, std::nullopt);
      auto const originalInfo = openedOriginal.concretePtr->streamInfo();
      auto const originalPcm = collectTailTestPcmBytes(*openedOriginal.concretePtr);
      REQUIRE_FALSE(originalPcm.empty());

      auto taggedBytes = tailTestAsciiBytes(ao::test::readFile(originalPath));
      appendTailTestId3v1Tag(taggedBytes);
      auto const temp = ao::test::TempFile{taggedBytes, ".mp3"};
      auto openedTagged = openTailTestSessions(temp.path, std::nullopt);
      auto const taggedInfo = openedTagged.concretePtr->streamInfo();
      CHECK(taggedInfo == originalInfo);
      CHECK(openedTagged.factoryPtr->streamInfo() == taggedInfo);
      auto const taggedPcm = collectTailTestPcmBytes(*openedTagged.concretePtr);
      CHECK(taggedPcm.size() == originalPcm.size());
      CHECK(std::ranges::equal(taggedPcm, originalPcm));

      // Equivalent seek history: the same interior seek on both streams must
      // yield the same remaining PCM instead of a new golden expectation.
      REQUIRE(openedOriginal.concretePtr->seek(std::chrono::milliseconds{100}));
      REQUIRE(openedTagged.concretePtr->seek(std::chrono::milliseconds{100}));
      auto const originalSoughtPcm = collectTailTestPcmBytes(*openedOriginal.concretePtr, firstFrameIndex);
      auto const taggedSoughtPcm = collectTailTestPcmBytes(*openedTagged.concretePtr, firstFrameIndex);
      CHECK(taggedSoughtPcm.size() == originalSoughtPcm.size());
      CHECK(std::ranges::equal(taggedSoughtPcm, originalSoughtPcm));
    }
  }
} // namespace ao::audio::test
