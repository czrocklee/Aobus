// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "DecoderTestSupport.h"
#include "lib/audio/FlacDecoderSession.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/FlacFrameTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/PcmBlock.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    constexpr std::uint32_t kBlockFrames = 16;
    constexpr std::size_t kStereoFrameBytes = 2U * sizeof(std::int16_t);
    constexpr std::size_t kId3v1Bytes = 128;
    constexpr auto kMarkers =
      std::array{std::array<std::int32_t, 2>{100, -200}, std::array<std::int32_t, 2>{300, -400}};
    constexpr std::uint64_t kAudioFrames = kBlockFrames * kMarkers.size();

    // APEv2 with one ordinary binary Palette item and no optional header.
    // clang-format off
    constexpr auto kApev2Tag = std::to_array<std::uint8_t>({
      3, 0, 0, 0, // Value length.
      2, 0, 0, 0, // Binary item flags.
      'P', 'a', 'l', 'e', 't', 't', 'e', 0,
      0x24, 0x10, 0x20, // RGB value.
      'A', 'P', 'E', 'T', 'A', 'G', 'E', 'X',
      0xD0, 0x07, 0, 0, // Version 2000.
      51, 0, 0, 0, // Size including the 32-byte footer.
      1, 0, 0, 0, // One item.
      0, 0, 0, 0, // Footer flags.
      0, 0, 0, 0, 0, 0, 0, 0, // Reserved.
    });
    // clang-format on
    static_assert(kApev2Tag.size() == 51);

    std::array<std::uint8_t, kId3v1Bytes> buildId3v1Tag()
    {
      auto tag = std::array<std::uint8_t, kId3v1Bytes>{};
      tag.fill(' ');
      std::ranges::copy(std::string_view{"TAGTone"}, tag.begin());
      std::ranges::copy(std::string_view{"2026Session take"}, tag.begin() + 93);
      // End the full ID3v1.0 comment with a Latin-1 sequence that looks like
      // frame sync. The one remaining byte is the genre (8, Jazz).
      tag[124] = 0xFF;
      tag[125] = 0xF8;
      tag[126] = 'y';
      tag[127] = 8;
      return tag;
    }

    std::vector<std::uint8_t> buildTaggedStream(std::uint64_t declaredFrames,
                                                std::span<std::uint8_t const> tag,
                                                bool hasGap = false)
    {
      auto stream = buildFlacStreaminfoBytes(declaredFrames);

      for (std::uint32_t index = 0; index < kMarkers.size(); ++index)
      {
        auto const frame = buildFlacVerbatimFrameBytes(index, 48000, 2, 16, kMarkers[index]);
        stream.insert(stream.end(), frame.begin(), frame.end());
      }

      if (hasGap)
      {
        stream.push_back('x');
      }

      stream.insert(stream.end(), tag.begin(), tag.end());
      return stream;
    }

    void checkAudio(FlacDecoderSession& decoder)
    {
      for (std::size_t index = 0; index < kMarkers.size(); ++index)
      {
        CAPTURE(index);
        auto const blockRes = decoder.readNextBlock();
        REQUIRE(blockRes);
        CHECK_FALSE(blockRes->endOfStream);
        CHECK(blockRes->firstFrameIndex == index * kBlockFrames);
        CHECK(blockRes->frames == kBlockFrames);
        REQUIRE(blockRes->bytes.size() == static_cast<std::size_t>(kBlockFrames) * kStereoFrameBytes);

        for (std::size_t frame = 0; frame < kBlockFrames; ++frame)
        {
          CHECK(readSigned16LePcmSample(blockRes->bytes, frame * 2U) == kMarkers[index][0]);
          CHECK(readSigned16LePcmSample(blockRes->bytes, (frame * 2U) + 1U) == kMarkers[index][1]);
        }
      }
    }

    void checkStableEmptyTagEnd(FlacDecoderSession& decoder)
    {
      for (std::uint32_t read = 0; read < 2; ++read)
      {
        auto const endRes = decoder.readNextBlock();
        REQUIRE(endRes);
        CHECK(endRes->endOfStream);
        CHECK(endRes->frames == 0);
        CHECK(endRes->bytes.empty());
      }
    }
  } // namespace

  TEST_CASE("FlacDecoderSession - adjacent trailing ID3v1 with sync-like bytes preserves PCM and EOF")
  {
    auto const declaredFrames = GENERATE(kAudioFrames, std::uint64_t{0});
    CAPTURE(declaredFrames);
    auto const temp = ao::test::TempFile{buildTaggedStream(declaredFrames, buildId3v1Tag()), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
    CHECK(decoderPtr->streamInfo().optTotalFrames.has_value() == (declaredFrames != 0));

    SECTION("Sequential reads deliver both complete audio blocks")
    {
      checkAudio(*decoderPtr);
    }

    if (declaredFrames != 0)
    {
      SECTION("Flush preserves the unread last sample from an exact seek")
      {
        constexpr std::uint64_t kLastSample = kAudioFrames - 1U;
        REQUIRE(decoderPtr->seekFrame(kLastSample));
        decoderPtr->flush();

        auto const tailRes = decoderPtr->readNextBlock();
        REQUIRE(tailRes);
        CHECK_FALSE(tailRes->endOfStream);
        CHECK(tailRes->firstFrameIndex == kLastSample);
        CHECK(tailRes->frames == 1);
        REQUIRE(tailRes->bytes.size() == kStereoFrameBytes);
        CHECK(readSigned16LePcmSample(tailRes->bytes, 0) == kMarkers.back()[0]);
        CHECK(readSigned16LePcmSample(tailRes->bytes, 1) == kMarkers.back()[1]);
      }
    }

    checkStableEmptyTagEnd(*decoderPtr);
  }

  TEST_CASE("FlacDecoderSession - adjacent trailing ID3v1 does not hide a declared sample deficit")
  {
    auto const temp = ao::test::TempFile{buildTaggedStream(kAudioFrames + kBlockFrames, buildId3v1Tag()), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));

    checkAudio(*decoderPtr);

    for (std::uint32_t read = 0; read < 2; ++read)
    {
      auto const endRes = decoderPtr->readNextBlock();
      REQUIRE_FALSE(endRes);
      CHECK(endRes.error().code == Error::Code::DecodeFailed);
      CHECK(endRes.error().message.contains("ended before the declared sample count"));
    }
  }

  TEST_CASE("FlacDecoderSession - sync-like trailing ID3v1 bytes fail without a marked adjacent tag")
  {
    auto tag = buildId3v1Tag();
    bool hasGap = false;

    SECTION("The suffix has no TAG marker")
    {
      tag[0] = 'X';
    }

    SECTION("The TAG marker is separated from the last audio block by junk")
    {
      hasGap = true;
    }

    // Both variants retain the exact same sync-like bytes at physical EOF.
    auto const temp = ao::test::TempFile{buildTaggedStream(kAudioFrames, tag, hasGap), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));

    checkAudio(*decoderPtr);
    auto const endRes = decoderPtr->readNextBlock();
    REQUIRE_FALSE(endRes);
    CHECK(endRes.error().code == Error::Code::DecodeFailed);
  }

  TEST_CASE("FlacDecoderSession - an apparent trailing ID3v1 tag does not hide valid audio")
  {
    auto const declaredFrames = GENERATE(kAudioFrames, std::uint64_t{0});
    CAPTURE(declaredFrames);
    auto stream = buildFlacStreaminfoBytes(declaredFrames);
    auto const firstFrame = buildFlacVerbatimFrameBytes(0, 48000, 2, 16, kMarkers.front());
    stream.insert(stream.end(), firstFrame.begin(), firstFrame.end());

    // The exact last 128 bytes begin with TAG at a frame boundary, but the
    // sync-free junk is followed by an intact frame that must still decode.
    constexpr auto kSignature = std::to_array<std::uint8_t>({'T', 'A', 'G'});
    auto const secondFrame = buildFlacVerbatimFrameBytes(1, 48000, 2, 16, kMarkers.back());
    REQUIRE(secondFrame.size() < kId3v1Bytes - kSignature.size());
    stream.insert(stream.end(), kSignature.begin(), kSignature.end());
    stream.insert(stream.end(), kId3v1Bytes - kSignature.size() - secondFrame.size(), std::uint8_t{' '});
    stream.insert(stream.end(), secondFrame.begin(), secondFrame.end());

    auto const temp = ao::test::TempFile{stream, ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
    checkAudio(*decoderPtr);
    checkStableEmptyTagEnd(*decoderPtr);
  }

  TEST_CASE("FlacDecoderSession - an ordinary trailing APEv2 item preserves PCM and EOF")
  {
    auto const temp = ao::test::TempFile{buildTaggedStream(kAudioFrames, kApev2Tag), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));

    checkAudio(*decoderPtr);
    checkStableEmptyTagEnd(*decoderPtr);
  }

  TEST_CASE("FlacDecoderSession - a trailing APEv2 item does not suppress a bad frame header error")
  {
    auto tag = kApev2Tag;
    // Put sync-like bytes at the end of the Palette value, just before the
    // footer. libFLAC reports BAD_HEADER, which must outrank the sample deficit.
    constexpr std::size_t kFooterBytes = 32;
    tag[tag.size() - kFooterBytes - 2U] = 0xFF;
    tag[tag.size() - kFooterBytes - 1U] = 0xF8;
    auto const temp = ao::test::TempFile{buildTaggedStream(kAudioFrames + kBlockFrames, tag), ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));

    checkAudio(*decoderPtr);
    auto const endRes = decoderPtr->readNextBlock();
    REQUIRE_FALSE(endRes);
    CHECK(endRes.error().code == Error::Code::DecodeFailed);
    CHECK(endRes.error().message.contains("BAD_HEADER"));
  }
} // namespace ao::audio::test
