// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "FlacFrameTestSupport.h"
#include "lib/audio/DecoderFactory.h"
#include "lib/audio/FlacDecoderSession.h"
#include "test/unit/TestFixtureSupport.h"
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

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    constexpr auto kMarkers16 = std::array{
      std::to_array<std::int32_t>({16384, -8192}),
      std::to_array<std::int32_t>({-16384, 8192}),
      std::to_array<std::int32_t>({8192, -4096}),
    };
    constexpr auto kMarkers24 = std::array{
      std::to_array<std::int32_t>({4194305, -2097153}),
      std::to_array<std::int32_t>({-4194305, 2097153}),
      std::to_array<std::int32_t>({2097153, -1048578}),
    };
    // Literal interleaved LE PCM, not produced by the FLAC writer or a decoder.
    // Both slots change each 16-sample frame; 24-bit markers retain low bits.
    constexpr auto kExpected16 = std::array{
      std::to_array<std::uint8_t>({0x00, 0x40, 0x00, 0xE0}),
      std::to_array<std::uint8_t>({0x00, 0xC0, 0x00, 0x20}),
      std::to_array<std::uint8_t>({0x00, 0x20, 0x00, 0xF0}),
    };
    constexpr auto kExpected24 = std::array{
      std::to_array<std::uint8_t>({0x01, 0x00, 0x40, 0xFF, 0xFF, 0xDF}),
      std::to_array<std::uint8_t>({0xFF, 0xFF, 0xBF, 0x01, 0x00, 0x20}),
      std::to_array<std::uint8_t>({0x01, 0x00, 0x20, 0xFE, 0xFF, 0xEF}),
    };

    void appendAudio(std::vector<std::uint8_t>& bytes, std::uint8_t precisionBits = 16)
    {
      for (std::uint32_t frameIndex = 0; frameIndex < 3U; ++frameIndex)
      {
        auto const markers = precisionBits == 24U ? std::span<std::int32_t const>{kMarkers24[frameIndex]}
                                                  : std::span<std::int32_t const>{kMarkers16[frameIndex]};
        auto const frame = buildFlacVerbatimFrameBytes(frameIndex, 48000, 2, precisionBits, markers);
        bytes.insert(bytes.end(), frame.begin(), frame.end());
      }
    }

    std::vector<std::uint8_t> buildCommentPayload(std::span<std::string const> comments)
    {
      // Reuse the ordinary writer for readable entries; only the tests below
      // alter internal lengths/counts. Do not duplicate its serialization.
      auto metadata = buildFlacStreaminfoBytes(48);
      auto const payloadOffset = metadata.size() + 4U;
      appendFlacCommentBlock(metadata, comments);
      return std::vector<std::uint8_t>{metadata.begin() + static_cast<std::ptrdiff_t>(payloadOffset), metadata.end()};
    }

    std::vector<std::uint8_t> buildUnreadableTailPayload(std::span<std::string const> prefix)
    {
      REQUIRE(prefix.size() < 255U);
      auto payload = buildCommentPayload(prefix);
      // The shared writer uses an empty vendor: count occupies bytes 4-7.
      // Claim one additional entry whose UINT32_MAX length exceeds the block.
      payload[4] = static_cast<std::uint8_t>(prefix.size() + 1U);
      payload.insert(payload.end(), {0xFF, 0xFF, 0xFF, 0xFF});
      return payload;
    }

    void checkIdentity(DecodedStreamInfo const& info,
                       std::uint32_t mask,
                       ChannelLayoutOrigin origin,
                       std::uint8_t precisionBits = 16)
    {
      auto const encoding = precisionBits == 24U ? SampleEncoding::Signed24PackedLe : SampleEncoding::Signed16Le;
      CHECK(info.sourceFormat == SignalFormat{.sampleRate = 48000,
                                              .channels = 2,
                                              .precisionBits = precisionBits,
                                              .sampleKind = SampleKind::Integer,
                                              .speakerMask = mask});
      CHECK(info.outputFormat ==
            PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = encoding, .speakerMask = mask});
      CHECK(info.sourceChannelLayout.origin == origin);
      CHECK(info.optTotalFrames == 48U);
      CHECK(info.duration == std::chrono::milliseconds{1});
      CHECK(info.codec == AudioCodec::Flac);
      CHECK_FALSE(info.isLossy);
    }

    void checkPcm(PcmBlock const& block,
                  std::uint64_t firstFrameIndex,
                  std::uint32_t frames,
                  std::size_t markerIndex,
                  std::uint8_t precisionBits = 16)
    {
      auto const expected = precisionBits == 24U ? std::span<std::uint8_t const>{kExpected24[markerIndex]}
                                                 : std::span<std::uint8_t const>{kExpected16[markerIndex]};
      CHECK(block.firstFrameIndex == firstFrameIndex);
      REQUIRE(block.frames == frames);
      REQUIRE(block.bytes.size() == frames * expected.size());
      CHECK_FALSE(block.endOfStream);

      for (std::size_t frame = 0; frame < frames; ++frame)
      {
        for (std::size_t byteIndex = 0; byteIndex < expected.size(); ++byteIndex)
        {
          CAPTURE(frame, byteIndex);
          CHECK(std::to_integer<std::uint8_t>(block.bytes[(frame * expected.size()) + byteIndex]) ==
                expected[byteIndex]);
        }
      }
    }

    void checkStableEof(DecoderSession& decoder)
    {
      auto const info = decoder.streamInfo();

      for (std::uint32_t repeat = 0; repeat < 3U; ++repeat)
      {
        auto const endRes = decoder.readNextBlock();
        REQUIRE(endRes);
        CHECK(endRes->endOfStream);
        CHECK(endRes->frames == 0U);
        CHECK(endRes->bytes.empty());
        CHECK(decoder.streamInfo() == info);
      }
    }

    void checkAllPcm(DecoderSession& decoder, std::uint8_t precisionBits = 16)
    {
      auto const info = decoder.streamInfo();

      for (std::size_t frameIndex = 0; frameIndex < 3U; ++frameIndex)
      {
        auto const blockRes = decoder.readNextBlock();
        REQUIRE(blockRes);
        checkPcm(*blockRes, frameIndex * 16U, 16, frameIndex, precisionBits);
        CHECK(decoder.streamInfo() == info);
      }

      checkStableEof(decoder);
    }

    void checkUnknownAdmission(std::vector<std::uint8_t> const& bytes)
    {
      auto const temp = ao::test::TempFile{bytes, ".flac"};
      auto concretePtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
      auto factoryPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
      checkIdentity(concretePtr->streamInfo(), 0, ChannelLayoutOrigin::Unknown);
      CHECK(factoryPtr->streamInfo() == concretePtr->streamInfo());
      checkAllPcm(*concretePtr);
      checkAllPcm(*factoryPtr);
    }
  } // namespace

  TEST_CASE("FlacDecoderSession - readable empty and unrelated comments identify the default layout",
            "[audio][integration][flac]")
  {
    auto const commentSets = std::vector<std::vector<std::string>>{
      {},
      {"TITLE=Unrelated metadata", "ARTIST=Fixture"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK_EXTRA=bad", " WAVEFORMATEXTENSIBLE_CHANNEL_MASK=bad"},
    };

    for (auto const& comments : commentSets)
    {
      CAPTURE(comments);
      auto bytes = buildFlacStreaminfoBytes(48);
      appendFlacCommentBlock(bytes, comments);
      appendAudio(bytes);
      auto const temp = ao::test::TempFile{bytes, ".flac"};
      auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
      checkIdentity(decoderPtr->streamInfo(), 3, ChannelLayoutOrigin::Identified);
      checkAllPcm(*decoderPtr);
    }
  }

  TEST_CASE("FlacDecoderSession - native ID3 prefix preserves comment admission despite embedded FLAC markers",
            "[audio][integration][flac]")
  {
    // Native ID3 skipping masks off the high bit of each size byte and can
    // begin after a partial fLaC match. Each payload still contains a marker.
    auto const prefixes = std::vector<std::vector<std::uint8_t>>{
      {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 4, 'f', 'L', 'a', 'C'},
      {'I', 'D', '3', 4, 0, 0, 0x80, 0x80, 0x80, 0x84, 'f', 'L', 'a', 'C'},
      {'f', 'L', 'I', 'D', '3', 4, 0, 0, 0, 0, 0, 4, 'f', 'L', 'a', 'C'},
    };

    for (bool const validMask : {false, true})
    {
      auto const comments = std::to_array<std::string>({
        "TITLE=Literal fLaC is comment data",
        validMask ? "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xC" : "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=bad",
      });
      auto payload = buildCommentPayload(comments);
      // Replace the empty vendor with four literal marker bytes, preserving
      // the entry count and lengths produced by the ordinary writer.
      payload[0] = 4;
      payload.insert(payload.begin() + 4, {'f', 'L', 'a', 'C'});
      auto stream = buildFlacStreaminfoBytes(48);
      appendFlacRawCommentBlock(stream, payload);
      appendAudio(stream);

      for (std::size_t prefixIndex = 0; prefixIndex < prefixes.size(); ++prefixIndex)
      {
        CAPTURE(validMask, prefixIndex);
        auto bytes = prefixes[prefixIndex];
        bytes.insert(bytes.end(), stream.begin(), stream.end());

        if (!validMask)
        {
          checkUnknownAdmission(bytes);
        }
        else
        {
          auto const temp = ao::test::TempFile{bytes, ".flac"};
          auto concretePtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
          auto factoryPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
          checkIdentity(concretePtr->streamInfo(), 0xC, ChannelLayoutOrigin::Identified);
          checkIdentity(factoryPtr->streamInfo(), 0xC, ChannelLayoutOrigin::Identified);
          checkAllPcm(*concretePtr);
          checkAllPcm(*factoryPtr);
        }
      }
    }
  }

  TEST_CASE("FlacDecoderSession - native prefix selection ignores invisible overlapping metadata chains",
            "[audio][integration][flac]")
  {
    auto fakeMetadata = buildFlacStreaminfoBytes(48);
    appendFlacCommentBlock(fakeMetadata, std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=bad"}));

    // This fake chain is metadata-only: no audio or raw frame-sync bytes may
    // turn it into an earlier native-selected stream while scanning prefixes.
    for (std::size_t byteIndex = 1; byteIndex < fakeMetadata.size(); ++byteIndex)
    {
      REQUIRE_FALSE((fakeMetadata[byteIndex - 1U] == 0xFFU && fakeMetadata[byteIndex] == 0xF8U));
    }

    auto stream = buildFlacStreaminfoBytes(48);
    appendFlacCommentBlock(stream, std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xC"}));
    appendAudio(stream);
    auto const prefixes = std::vector<std::vector<std::uint8_t>>{
      {'f'},
      {'f', 'L'},
      {'f', 'L', 'a'},
      {0xFF},
      {0xFF, 0xFF},
    };

    for (std::size_t prefixIndex = 0; prefixIndex < prefixes.size(); ++prefixIndex)
    {
      CAPTURE(prefixIndex);
      // Native matching does not reprocess the second f in ffLaC. Its FF
      // sync probe also consumes the fake marker's f, including repeated FF.
      // A plain substring search would instead select this known-bad chain.
      auto bytes = prefixes[prefixIndex];
      bytes.insert(bytes.end(), fakeMetadata.begin(), fakeMetadata.end());
      bytes.insert(bytes.end(), stream.begin(), stream.end());
      auto const temp = ao::test::TempFile{bytes, ".flac"};
      auto concretePtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
      auto factoryPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
      checkIdentity(concretePtr->streamInfo(), 0xC, ChannelLayoutOrigin::Identified);
      checkIdentity(factoryPtr->streamInfo(), 0xC, ChannelLayoutOrigin::Identified);
      checkAllPcm(*concretePtr);
      checkAllPcm(*factoryPtr);
    }
  }

  TEST_CASE("FlacDecoderSession - repeated STREAMINFO cannot hide readable mask declarations",
            "[audio][integration][flac]")
  {
    auto const commentSets = std::vector<std::vector<std::string>>{
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xC"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=bad"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x0"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3", "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xC"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x7"},
    };
    auto const streamInfo = buildFlacStreaminfoBytes(48);

    for (std::size_t index = 0; index < commentSets.size(); ++index)
    {
      CAPTURE(index, commentSets[index]);
      auto bytes = streamInfo;
      // Keep one marker, copy the complete independently authored SI block,
      // and clear the first SI's last flag. Appending VC clears the second
      // SI's last flag too, leaving two identical non-last SI blocks.
      bytes[4] &= 0x7FU;
      bytes.insert(bytes.end(), streamInfo.begin() + 4, streamInfo.end());
      appendFlacCommentBlock(bytes, commentSets[index]);
      appendAudio(bytes);

      if (index != 0U)
      {
        checkUnknownAdmission(bytes);
      }
      else
      {
        auto const temp = ao::test::TempFile{bytes, ".flac"};
        auto concretePtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
        auto factoryPtr = ao::test::requireValue(openDecoderSession(temp.path, std::nullopt));
        checkIdentity(concretePtr->streamInfo(), 0xC, ChannelLayoutOrigin::Identified);
        checkIdentity(factoryPtr->streamInfo(), 0xC, ChannelLayoutOrigin::Identified);
        checkAllPcm(*concretePtr);
        checkAllPcm(*factoryPtr);
      }
    }
  }

  TEST_CASE("FlacDecoderSession - non-first STREAMINFO cannot hide early masks behind padding markers",
            "[audio][integration][flac]")
  {
    auto const earlyCommentSets = std::vector<std::vector<std::string>>{
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=bad"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x0"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xC"},
    };
    auto const laterGood = std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xC"});
    auto const streamInfo = buildFlacStreaminfoBytes(48);

    for (auto const& earlyComments : earlyCommentSets)
    {
      for (bool const hasId3Prefix : {false, true})
      {
        CAPTURE(earlyComments, hasId3Prefix);
        auto earlyMetadata = streamInfo;
        appendFlacCommentBlock(earlyMetadata, earlyComments);
        // Move the builder's complete SI after the early VC and PADDING:
        // retain the initial marker and VC bytes, but not its original SI.
        auto bytes = std::vector<std::uint8_t>{earlyMetadata.begin(), earlyMetadata.begin() + 4};
        bytes.insert(
          bytes.end(), earlyMetadata.begin() + static_cast<std::ptrdiff_t>(streamInfo.size()), earlyMetadata.end());
        bytes[4] &= 0x7FU; // The early VC is no longer the last block.
        auto const padding = std::to_array<std::uint8_t>({0x01, 0, 0, 4, 'f', 'L', 'a', 'C'});
        bytes.insert(bytes.end(), padding.begin(), padding.end());
        bytes.insert(bytes.end(), streamInfo.begin() + 4, streamInfo.end());
        // This clears the moved SI's last flag. The fake marker at the end
        // of padding immediately precedes SI, but is not the chain's marker.
        appendFlacCommentBlock(bytes, laterGood);
        appendAudio(bytes);

        if (hasId3Prefix)
        {
          // Native ID3v2.4 skipping must also ignore this earlier fake marker.
          auto const prefix = std::to_array<std::uint8_t>({'I', 'D', '3', 4, 0, 0, 0, 0, 0, 4, 'f', 'L', 'a', 'C'});
          bytes.insert(bytes.begin(), prefix.begin(), prefix.end());
        }

        // No non-first SI chain is trustworthy, even when declarations agree.
        // A padding-tail marker must not manufacture a first-SI suffix.
        checkUnknownAdmission(bytes);
      }
    }
  }

  TEST_CASE("FlacDecoderSession - an early-only valid mask before STREAMINFO remains unknown",
            "[audio][integration][flac]")
  {
    auto const streamInfo = buildFlacStreaminfoBytes(48);
    auto earlyMetadata = streamInfo;
    appendFlacCommentBlock(earlyMetadata, std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xC"}));
    // Clear the authored VC header before copying it after the marker. It will
    // precede the last STREAMINFO block in every independently assembled row.
    earlyMetadata.at(streamInfo.size()) &= 0x7FU;

    for (bool const hasId3Prefix : {false, true})
    {
      for (bool const hasPaddingMarker : {false, true})
      {
        CAPTURE(hasId3Prefix, hasPaddingMarker);
        auto bytes = std::vector<std::uint8_t>{earlyMetadata.begin(), earlyMetadata.begin() + 4};
        bytes.insert(
          bytes.end(), earlyMetadata.begin() + static_cast<std::ptrdiff_t>(streamInfo.size()), earlyMetadata.end());

        if (hasPaddingMarker)
        {
          auto const padding = std::to_array<std::uint8_t>({0x01, 0, 0, 4, 'f', 'L', 'a', 'C'});
          bytes.insert(bytes.end(), padding.begin(), padding.end());
        }

        // No later declaration exists. Neither the early valid mask nor a
        // padding-tail marker can replace the required first-block SI anchor.
        bytes.insert(bytes.end(), streamInfo.begin() + 4, streamInfo.end());
        appendAudio(bytes);

        if (hasId3Prefix)
        {
          auto const prefix = std::to_array<std::uint8_t>({'I', 'D', '3', 4, 0, 0, 0, 0, 0, 4, 'f', 'L', 'a', 'C'});
          bytes.insert(bytes.begin(), prefix.begin(), prefix.end());
        }

        checkUnknownAdmission(bytes);
      }
    }
  }

  TEST_CASE("FlacDecoderSession - 100000 and 100001 complete unrelated comments preserve all audio",
            "[audio][integration][flac]")
  {
    // libFLAC 1.5.0 rejects the second payload when VC response is enabled;
    // this format-valid count must not become an application admission cap.
    for (std::size_t const count : {100000U, 100001U})
    {
      CAPTURE(count);
      auto const comments = std::vector<std::string>(count, "X=Y");
      auto bytes = buildFlacStreaminfoBytes(48);
      appendFlacCommentBlock(bytes, comments);
      appendAudio(bytes);
      auto const temp = ao::test::TempFile{bytes, ".flac"};
      auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
      checkIdentity(decoderPtr->streamInfo(), 3, ChannelLayoutOrigin::Identified);
      checkAllPcm(*decoderPtr);
    }
  }

  TEST_CASE("FlacDecoderSession - explicit mask after 100001 comments is still recognized",
            "[audio][integration][flac]")
  {
    auto comments = std::vector<std::string>(100001, "X=Y");
    comments.emplace_back("WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xC");
    auto bytes = buildFlacStreaminfoBytes(48, 2, 24);
    appendFlacCommentBlock(bytes, comments);
    appendAudio(bytes, 24);
    auto const temp = ao::test::TempFile{bytes, ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
    checkIdentity(decoderPtr->streamInfo(), 0xC, ChannelLayoutOrigin::Identified, 24);
    checkAllPcm(*decoderPtr, 24);
  }

  TEST_CASE("FlacDecoderSession - internally damaged comment blocks play with unknown identity",
            "[audio][integration][flac]")
  {
    auto trailing = buildCommentPayload(std::to_array<std::string>({"TITLE=Readable"}));
    trailing.push_back(0);
    auto const payloads = std::vector<std::vector<std::uint8_t>>{
      trailing,
      // Empty vendor, one entry, length 4 but only 3 bytes remain.
      {0, 0, 0, 0, 1, 0, 0, 0, 4, 0, 0, 0, 'X', '=', 'Y'},
      {0, 0, 0, 0, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 'X', '=', 'Y'},
      // Vendor length itself overruns the bounded payload.
      {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0},
      {},
      {0, 0, 0, 0, 0, 0, 0},
      // UINT32_MAX entries but no entry lengths/data follow.
      {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF},
      // Incomplete entry-length field, and a vendor leaving no full count.
      {0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},
      {1, 0, 0, 0, 'v', 0, 0, 0},
    };

    for (std::size_t index = 0; index < payloads.size(); ++index)
    {
      CAPTURE(index);
      auto bytes = buildFlacStreaminfoBytes(48);
      appendFlacRawCommentBlock(bytes, payloads[index]);
      appendAudio(bytes);
      auto const temp = ao::test::TempFile{bytes, ".flac"};
      auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
      checkIdentity(decoderPtr->streamInfo(), 0, ChannelLayoutOrigin::Unknown);
      checkAllPcm(*decoderPtr);
    }
  }

  TEST_CASE("FlacDecoderSession - readable valid mask cannot identify a damaged comment remainder",
            "[audio][integration][flac]")
  {
    auto const comments = std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xC"});

    for (bool const unreadableEntry : {false, true})
    {
      CAPTURE(unreadableEntry);
      auto payload = unreadableEntry ? buildUnreadableTailPayload(comments) : buildCommentPayload(comments);

      if (!unreadableEntry)
      {
        payload.push_back(0x7F); // Outside the declared, fully readable list.
      }

      auto bytes = buildFlacStreaminfoBytes(48);
      appendFlacRawCommentBlock(bytes, payload);
      appendAudio(bytes);
      auto const temp = ao::test::TempFile{bytes, ".flac"};
      auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
      checkIdentity(decoderPtr->streamInfo(), 0, ChannelLayoutOrigin::Unknown);
      checkAllPcm(*decoderPtr);
    }
  }

  TEST_CASE("FlacDecoderSession - later good comment blocks cannot erase structural uncertainty",
            "[audio][integration][flac]")
  {
    auto const good = std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xC"});
    auto const damaged = std::to_array<std::uint8_t>({0, 0, 0, 0, 0, 0, 0});

    for (bool const goodFirst : {false, true})
    {
      CAPTURE(goodFirst);
      auto bytes = buildFlacStreaminfoBytes(48);

      if (goodFirst)
      {
        appendFlacCommentBlock(bytes, good);
        appendFlacRawCommentBlock(bytes, damaged);
      }
      else
      {
        appendFlacRawCommentBlock(bytes, damaged);
        appendFlacCommentBlock(bytes, good);
      }

      appendAudio(bytes);
      auto const temp = ao::test::TempFile{bytes, ".flac"};
      auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
      checkIdentity(decoderPtr->streamInfo(), 0, ChannelLayoutOrigin::Unknown);
      checkAllPcm(*decoderPtr);
    }
  }

  TEST_CASE("FlacDecoderSession - invalid declarations keep identity unknown across complete or damaged tails",
            "[audio][integration][flac]")
  {
    auto const badCommentSets = std::vector<std::vector<std::string>>{
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xCjunk"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x0"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x1"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x7"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x40003"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3", "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xC"},
    };
    auto const good = std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xC"});

    for (auto const& comments : badCommentSets)
    {
      for (bool const damagedTail : {false, true})
      {
        for (bool const laterGood : {false, true})
        {
          CAPTURE(comments, damagedTail, laterGood);
          auto bytes = buildFlacStreaminfoBytes(48);

          if (damagedTail)
          {
            appendFlacRawCommentBlock(bytes, buildUnreadableTailPayload(comments));
          }
          else
          {
            appendFlacCommentBlock(bytes, comments);
          }

          if (laterGood)
          {
            appendFlacCommentBlock(bytes, good);
          }

          appendAudio(bytes);
          checkUnknownAdmission(bytes);
        }
      }
    }
  }

  TEST_CASE("FlacDecoderSession - later declarations cannot restore identity after structural uncertainty",
            "[audio][integration][flac]")
  {
    auto const good = std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xC"});
    auto const badCommentSets = std::vector<std::vector<std::string>>{
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=bad"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x0"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x1"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x7"},
      {"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x40003"},
    };

    for (auto const& comments : badCommentSets)
    {
      CAPTURE(comments);
      auto bytes = buildFlacStreaminfoBytes(48);
      appendFlacRawCommentBlock(bytes, std::to_array<std::uint8_t>({0, 0, 0, 0, 0, 0, 0}));
      appendFlacCommentBlock(bytes, comments);
      appendFlacCommentBlock(bytes, good);
      appendAudio(bytes);
      checkUnknownAdmission(bytes);
    }

    // An impossible entry count with a readable bad prefix remains Unknown;
    // a later complete good block must not restore identity.
    auto hugeCountPayload =
      buildUnreadableTailPayload(std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=bad"}));

    for (std::size_t byteIndex = 4; byteIndex < 8U; ++byteIndex)
    {
      hugeCountPayload[byteIndex] = 0xFF;
    }

    auto hugeCountBytes = buildFlacStreaminfoBytes(48);
    appendFlacRawCommentBlock(hugeCountBytes, hugeCountPayload);
    appendFlacCommentBlock(hugeCountBytes, good);
    appendAudio(hugeCountBytes);
    checkUnknownAdmission(hugeCountBytes);

    // A valid prefix with an unreadable remainder stays Unknown even when
    // the next complete block would otherwise supply an identified mask.
    auto bytes = buildFlacStreaminfoBytes(48);
    appendFlacRawCommentBlock(
      bytes, buildUnreadableTailPayload(std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3"})));
    appendFlacCommentBlock(bytes, good);
    appendAudio(bytes);
    checkUnknownAdmission(bytes);
  }

  TEST_CASE("FlacDecoderSession - unknown identity survives seek flush EOF and factory admission",
            "[audio][integration][flac]")
  {
    for (std::uint8_t const precisionBits : std::to_array<std::uint8_t>({16, 24}))
    {
      for (bool const damagedComment : {false, true})
      {
        CAPTURE(precisionBits, damagedComment);
        auto bytes = buildFlacStreaminfoBytes(48, 2, precisionBits);

        if (damagedComment)
        {
          appendFlacRawCommentBlock(
            bytes, buildUnreadableTailPayload(std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0xC"})));
        }
        else
        {
          appendFlacCommentBlock(bytes, std::to_array<std::string>({"WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x1"}));
        }

        appendAudio(bytes, precisionBits);
        auto const temp = ao::test::TempFile{bytes, ".flac"};
        auto decoders = std::array<std::unique_ptr<DecoderSession>, 2>{
          ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt)),
          ao::test::requireValue(openDecoderSession(temp.path, std::nullopt)),
        };
        CHECK(decoders[0]->streamInfo() == decoders[1]->streamInfo());

        for (auto const& decoderPtr : decoders)
        {
          auto& decoder = *decoderPtr;
          auto const info = decoder.streamInfo();
          checkIdentity(info, 0, ChannelLayoutOrigin::Unknown, precisionBits);
          CHECK(decoder.supportsFrameSeek());
          auto const firstRes = decoder.readNextBlock();
          REQUIRE(firstRes);
          checkPcm(*firstRes, 0, 16, 0, precisionBits);
          decoder.flush();
          CHECK(decoder.streamInfo() == info);
          auto const middleRes = decoder.readNextBlock();
          REQUIRE(middleRes);
          checkPcm(*middleRes, 16, 16, 1, precisionBits);
          CHECK(decoder.streamInfo() == info);

          REQUIRE(decoder.seekFrame(21));
          CHECK(decoder.streamInfo() == info);
          decoder.flush();
          auto const soughtRes = decoder.readNextBlock();
          REQUIRE(soughtRes);
          checkPcm(*soughtRes, 21, 11, 1, precisionBits);
          CHECK(decoder.streamInfo() == info);
          auto const lastRes = decoder.readNextBlock();
          REQUIRE(lastRes);
          checkPcm(*lastRes, 32, 16, 2, precisionBits);
          CHECK(decoder.streamInfo() == info);
          checkStableEof(decoder);

          REQUIRE(decoder.seekFrame(0));
          CHECK(decoder.streamInfo() == info);
          checkAllPcm(decoder, precisionBits);
        }
      }
    }
  }

  TEST_CASE("FlacDecoderSession - raw frames without STREAMINFO fail with the required metadata diagnostic",
            "[audio][integration][flac]")
  {
    for (bool const hasJunkPrefix : {false, true})
    {
      CAPTURE(hasJunkPrefix);
      auto bytes = std::vector<std::uint8_t>{};

      if (hasJunkPrefix)
      {
        bytes.insert(bytes.end(), {'j', 'u', 'n', 'k'});
      }

      // Native sync can enter the write callback on the first process call.
      // Valid frame geometry is not a substitute for required STREAMINFO.
      appendAudio(bytes);
      auto const temp = ao::test::TempFile{bytes, ".flac"};
      auto const concreteRes = FlacDecoderSession::open(temp.path, std::nullopt);
      auto const factoryRes = openDecoderSession(temp.path, std::nullopt);
      REQUIRE_FALSE(concreteRes);
      REQUIRE_FALSE(factoryRes);
      CHECK(concreteRes.error().code == Error::Code::DecodeFailed);
      CHECK(factoryRes.error().code == Error::Code::DecodeFailed);
      CHECK(concreteRes.error().message.contains("FLAC stream is missing valid STREAMINFO"));
      CHECK(factoryRes.error().message.contains("FLAC stream is missing valid STREAMINFO"));
    }
  }

  TEST_CASE("FlacDecoderSession - truncated native metadata remains a hard open failure", "[audio][integration][flac]")
  {
    auto bytes = buildFlacStreaminfoBytes(48);

    SECTION("First STREAMINFO is incomplete before any metadata can be published")
    {
      bytes.pop_back();
    }

    SECTION("A later padding payload is incomplete after a valid first STREAMINFO")
    {
      bytes[4] &= 0x7FU;
      bytes.insert(bytes.end(), {0x81, 0, 0, 4, 0, 0, 0});
    }

    SECTION("Non-last STREAMINFO cannot end without another block header")
    {
      bytes[4] &= 0x7FU;
    }

    SECTION("A following metadata header is truncated")
    {
      bytes[4] &= 0x7FU;
      bytes.insert(bytes.end(), {0x81, 0, 0});
    }

    SECTION("A skipped unknown metadata block still requires its complete payload")
    {
      bytes[4] &= 0x7FU;
      bytes.insert(bytes.end(), {0x87, 0, 0, 4, 0, 0, 0});
    }

    SECTION("A duplicate STREAMINFO cannot hide its missing payload")
    {
      bytes[4] &= 0x7FU;
      bytes.insert(bytes.end(), {0x80, 0, 0, 0});
    }

    SECTION("Unknown sample count does not relax native comment block boundaries")
    {
      bytes = buildFlacStreaminfoBytes(0);
      bytes[4] &= 0x7FU;
      bytes.insert(bytes.end(), {0x84, 0, 0, 8, 0, 0, 0});
    }

    // EOF while consuming a metadata payload can make native processing return
    // false without an error callback. It must never degrade to optional Unknown.
    auto const temp = ao::test::TempFile{bytes, ".flac"};
    auto const concreteRes = FlacDecoderSession::open(temp.path, std::nullopt);
    auto const factoryRes = openDecoderSession(temp.path, std::nullopt);
    REQUIRE_FALSE(concreteRes);
    REQUIRE_FALSE(factoryRes);
    CHECK(concreteRes.error().code == Error::Code::DecodeFailed);
    CHECK(factoryRes.error().code == Error::Code::DecodeFailed);
    CHECK(concreteRes.error().message.contains("Failed to read FLAC metadata"));
    CHECK(factoryRes.error().message.contains("Failed to read FLAC metadata"));
  }

  TEST_CASE("FlacDecoderSession - comment tolerance does not hide outer metadata or STREAMINFO damage",
            "[audio][integration][flac]")
  {
    auto bytes = buildFlacStreaminfoBytes(48);
    auto const commentHeader = bytes.size();
    appendFlacRawCommentBlock(bytes, std::to_array<std::uint8_t>({0, 0, 0, 0, 0, 0, 0}));
    appendAudio(bytes);

    SECTION("Declared metadata payload extends beyond the whole file")
    {
      // The actual frames are much shorter than this declaration, so they
      // cannot accidentally satisfy the outer block length by resynchronizing.
      bytes[commentHeader + 1U] = 0xFF;
      bytes[commentHeader + 2U] = 0xFF;
      bytes[commentHeader + 3U] = 0xFF;
    }

    SECTION("Metadata block header itself is truncated")
    {
      bytes.resize(commentHeader + 2U);
    }

    SECTION("STREAMINFO sample rate is zero")
    {
      bytes[18] = 0;
      bytes[19] = 0;
      bytes[20] &= 0x0FU;
    }

    auto const temp = ao::test::TempFile{bytes, ".flac"};
    auto const concreteRes = FlacDecoderSession::open(temp.path, std::nullopt);
    auto const factoryRes = openDecoderSession(temp.path, std::nullopt);
    REQUIRE_FALSE(concreteRes);
    REQUIRE_FALSE(factoryRes);
    CHECK(concreteRes.error().code == Error::Code::DecodeFailed);
    CHECK(factoryRes.error().code == Error::Code::DecodeFailed);
  }

  TEST_CASE("FlacDecoderSession - damaged comments cannot conceal contradictory audio frames",
            "[audio][integration][flac]")
  {
    auto bytes = buildFlacStreaminfoBytes(48);
    appendFlacRawCommentBlock(bytes, std::to_array<std::uint8_t>({0, 0, 0, 0, 0, 0, 0}));
    // Append a CRC-valid but wrong-rate frame, not arbitrary noise that the
    // provider could skip while finding a later intact frame.
    auto const first = buildFlacVerbatimFrameBytes(0, 48000, 2, 16, kMarkers16[0]);
    auto const wrong = buildFlacVerbatimFrameBytes(1, 44100, 2, 16, kMarkers16[1]);
    bytes.insert(bytes.end(), first.begin(), first.end());
    bytes.insert(bytes.end(), wrong.begin(), wrong.end());
    auto const temp = ao::test::TempFile{bytes, ".flac"};
    auto decoderPtr = ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt));
    auto const info = decoderPtr->streamInfo();
    checkIdentity(info, 0, ChannelLayoutOrigin::Unknown);
    auto const firstRes = decoderPtr->readNextBlock();
    REQUIRE(firstRes);
    checkPcm(*firstRes, 0, 16, 0);

    for (std::uint32_t repeat = 0; repeat < 3U; ++repeat)
    {
      auto const readRes = decoderPtr->readNextBlock();
      REQUIRE_FALSE(readRes);
      CHECK(readRes.error().code == Error::Code::DecodeFailed);
      CHECK(readRes.error().message.contains("44100"));
      CHECK(decoderPtr->streamInfo() == info);
    }
  }
} // namespace ao::audio::test
