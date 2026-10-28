// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/media/file/mpeg/Frame.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/media/file/TestFile.h"
#include <ao/library/AudioIdentity.h>
#include <ao/utility/Hash128.h>
#include <ao/utility/Xxh3.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace ao::media::file::mpeg::test
{
  using TestFile = ao::media::file::test::TestFile;
  using namespace ao::test;

  namespace
  {
    constexpr std::size_t kId3v1TagBytes = 128;
    // MPEG1 Layer III at 128 kbit/s and 44100 Hz: 144 * 128000 / 44100 bytes.
    constexpr std::size_t kCdRateFrameBytes = 417;
    // Third header byte: bitrate index 9 (128 kbit/s), 44100 Hz, no padding.
    constexpr std::uint8_t kCdRateBitrateRateByte = 0x90;
    constexpr std::size_t kChainFrameCount = 5;
    constexpr std::size_t kEncodedTailMissingBytes = 117;

    std::vector<std::uint8_t> createMpegFrame(std::size_t frameBytes, std::uint8_t bitrateRateByte)
    {
      auto frame = std::vector<std::uint8_t>(frameBytes, 0);
      frame[0] = 0xFF;
      frame[1] = 0xFB;
      frame[2] = bitrateRateByte;
      return frame;
    }

    std::vector<std::uint8_t> createFrameChain(std::vector<std::uint8_t> const& frame, std::size_t frameCount)
    {
      auto data = std::vector<std::uint8_t>{};
      data.reserve(frameCount * frame.size());

      for (std::size_t index = 0; index < frameCount; ++index)
      {
        data.insert_range(data.end(), frame);
      }

      return data;
    }

    // A 128-byte ID3v1 record with printable fields; the title must fit the
    // 30-byte field.
    std::vector<std::uint8_t> createId3v1Tag(std::string_view title)
    {
      auto tag = std::vector<std::uint8_t>(kId3v1TagBytes, 0);
      std::memcpy(tag.data(), "TAG", 3);
      std::memcpy(tag.data() + 3, title.data(), title.size());
      std::memcpy(tag.data() + 33, "Artist", 6);
      std::memcpy(tag.data() + 63, "Album", 5);
      std::memcpy(tag.data() + 93, "2026", 4);
      std::memcpy(tag.data() + 97, "Comment", 7);
      return tag;
    }

    // Five complete frames plus a fragment of a sixth, followed by a genuine
    // ID3v1 tag whose marker sits exactly at physical EOF - 128.
    std::vector<std::uint8_t> createTruncatedTailFile(std::size_t fragmentBytes, std::string_view title)
    {
      auto const frame = createMpegFrame(kCdRateFrameBytes, kCdRateBitrateRateByte);
      auto data = createFrameChain(frame, kChainFrameCount);
      data.insert_range(data.end(), std::span<std::uint8_t const>{frame}.first(fragmentBytes));
      data.insert_range(data.end(), createId3v1Tag(title));
      return data;
    }

    std::span<std::byte const> asFileBytes(std::vector<std::uint8_t> const& data)
    {
      return std::as_bytes(std::span<std::uint8_t const>{data});
    }

    // The payload the historical ID3v1 crop selects: the file bytes before the tag.
    std::span<std::byte const> bytesBeforeTrailingTag(std::vector<std::uint8_t> const& data)
    {
      return asFileBytes(data).first(data.size() - kId3v1TagBytes);
    }

    struct PayloadSnapshot final
    {
      std::size_t offset = 0;
      std::vector<std::byte> bytes{};
      utility::Hash128 hash{};
    };

    PayloadSnapshot readAudioPayload(std::vector<std::uint8_t> const& data)
    {
      auto const temp = TempFile{data, ".mp3"};
      auto const file = TestFile{temp.path};
      auto payloadRes = file.audioPayload();
      REQUIRE(payloadRes);
      return PayloadSnapshot{
        .offset = payloadRes->offset,
        .bytes = {payloadRes->bytes.begin(), payloadRes->bytes.end()},
        .hash = utility::xxh3Hash128(payloadRes->bytes),
      };
    }
  } // namespace

  TEST_CASE("MPEG File - crops a real trailing ID3v1 tag when no complete frame chain contains it")
  {
    // A 129-byte deficit leaves the declared frame uncompletable inside the
    // file. Deficits of 117 bytes and below borrow tag bytes to complete the
    // declared frame but leave four or more tag bytes after it, and the walk
    // does not resynchronize through the tag. Deficit 0 is the genuine-tag-
    // after-complete-frames control. Every row takes the historical ID3v1
    // crop and stays title-invariant.
    struct CropCase final
    {
      std::size_t missingBytes;
      std::size_t expectedPayloadBytes;
    };

    auto const cases = std::to_array<CropCase>({
      {.missingBytes = 129, .expectedPayloadBytes = 2373},
      {.missingBytes = 117, .expectedPayloadBytes = 2385},
      {.missingBytes = 104, .expectedPayloadBytes = 2398},
      {.missingBytes = 17, .expectedPayloadBytes = 2485},
      {.missingBytes = 1, .expectedPayloadBytes = 2501},
      {.missingBytes = 0, .expectedPayloadBytes = 2502},
    });

    for (auto const& cropCase : cases)
    {
      CAPTURE(cropCase.missingBytes);
      auto const fragmentBytes = kCdRateFrameBytes - cropCase.missingBytes;
      auto const beforeData = createTruncatedTailFile(fragmentBytes, "Before");
      auto const afterData = createTruncatedTailFile(fragmentBytes, "After!");
      auto const before = readAudioPayload(beforeData);
      auto const after = readAudioPayload(afterData);

      CHECK(before.bytes.size() == cropCase.expectedPayloadBytes);
      CHECK(after.bytes.size() == cropCase.expectedPayloadBytes);
      CHECK(before.hash == after.hash);
      CHECK(std::ranges::equal(before.bytes, bytesBeforeTrailingTag(beforeData)));
    }
  }

  TEST_CASE("MPEG File - retains a tag-completed frame chain ending within three bytes of the file end")
  {
    // Deficits 125..128 complete the declared frame using tag bytes and leave
    // 0..3 tolerated extra bytes before the file end. The policy deliberately
    // retains the borrowed bytes, so each deficit selects a different
    // boundary and the payload stays title-sensitive: this is the accepted
    // residual ambiguity, not a provenance guarantee.
    struct RetainCase final
    {
      std::size_t missingBytes;
      std::size_t expectedPayloadBytes;
    };

    auto const cases = std::to_array<RetainCase>({
      {.missingBytes = 128, .expectedPayloadBytes = 2502},
      {.missingBytes = 127, .expectedPayloadBytes = 2503},
      {.missingBytes = 126, .expectedPayloadBytes = 2504},
      {.missingBytes = 125, .expectedPayloadBytes = 2505},
    });

    for (auto const& retainCase : cases)
    {
      CAPTURE(retainCase.missingBytes);
      auto const fragmentBytes = kCdRateFrameBytes - retainCase.missingBytes;
      auto const beforeData = createTruncatedTailFile(fragmentBytes, "Before");
      auto const afterData = createTruncatedTailFile(fragmentBytes, "After!");
      auto const before = readAudioPayload(beforeData);
      auto const after = readAudioPayload(afterData);

      CHECK(before.bytes.size() == retainCase.expectedPayloadBytes);
      CHECK(after.bytes.size() == retainCase.expectedPayloadBytes);
      CHECK(std::ranges::equal(before.bytes, asFileBytes(beforeData)));
      CHECK(before.hash != after.hash);
    }
  }

  TEST_CASE("MPEG File - retitling a truncated-tail file keeps the audio payload identity")
  {
    // The known regression: five complete frames plus a 300-byte fragment
    // borrow 117 tag bytes to complete the declared frame, but eleven tag
    // bytes then remain unaccounted for, so the tag must be cropped.
    auto const beforeData = createTruncatedTailFile(300, "Before");
    auto const afterData = createTruncatedTailFile(300, "After!");
    auto const before = readAudioPayload(beforeData);
    auto const after = readAudioPayload(afterData);

    CHECK(before.offset == 0);
    CHECK(before.bytes.size() == 2385);
    CHECK(after.bytes.size() == 2385);
    CHECK(std::ranges::equal(before.bytes, bytesBeforeTrailingTag(beforeData)));
    CHECK(std::ranges::equal(after.bytes, bytesBeforeTrailingTag(afterData)));
    CHECK(before.hash == after.hash);

    // The persisted encoded-audio identity is title-invariant through the
    // library reader, not only through raw payload comparison.
    auto const optBeforeIdentity = library::readAudioIdentity(std::span<std::byte const>{before.bytes});
    auto const optAfterIdentity = library::readAudioIdentity(std::span<std::byte const>{after.bytes});
    REQUIRE(optBeforeIdentity);
    REQUIRE(optAfterIdentity);
    CHECK(optBeforeIdentity->payloadLength == 2385);
    CHECK(optBeforeIdentity->signature == optAfterIdentity->signature);

    // Borrowing the tag would report a 157 ms duration instead.
    auto const temp = TempFile{beforeData, ".mp3"};
    auto const file = TestFile{temp.path};
    auto contentRes = file.readContent();
    REQUIRE(contentRes);
    CHECK(contentRes->duration() == std::chrono::milliseconds{149});
  }

  TEST_CASE("MPEG File - retitling an encoded fixture with a truncated tail keeps the payload identity")
  {
    // The real fixture mixes table bitrates, so the audio extent is selected
    // through the public payload instead of an assumed uniform frame geometry.
    auto const fixtureFile = TestFile{audio::test::requireAudioFixture("basic_metadata.mp3")};
    auto fixturePayloadRes = fixtureFile.audioPayload();
    REQUIRE(fixturePayloadRes);
    REQUIRE(fixturePayloadRes->bytes.size() > kEncodedTailMissingBytes);

    auto const originalBytes = std::vector<std::byte>{fixturePayloadRes->bytes.begin(), fixturePayloadRes->bytes.end()};
    auto const truncatedBytes = originalBytes.size() - kEncodedTailMissingBytes;
    auto buildTruncatedTailFile = [&](std::string_view title)
    {
      auto data = std::vector<std::uint8_t>(truncatedBytes);
      std::memcpy(data.data(), originalBytes.data(), truncatedBytes);
      data.insert_range(data.end(), createId3v1Tag(title));
      return data;
    };

    auto const before = readAudioPayload(buildTruncatedTailFile("Before"));
    auto const after = readAudioPayload(buildTruncatedTailFile("After!"));

    CHECK(before.offset == 0);
    CHECK(before.bytes.size() == truncatedBytes);
    CHECK(after.bytes.size() == truncatedBytes);
    auto const expectedAudio = std::span<std::byte const>{originalBytes}.first(truncatedBytes);
    CHECK(std::ranges::equal(before.bytes, expectedAudio));
    CHECK(std::ranges::equal(after.bytes, expectedAudio));
    CHECK(before.hash == after.hash);

    auto const optBeforeIdentity = library::readAudioIdentity(std::span<std::byte const>{before.bytes});
    auto const optAfterIdentity = library::readAudioIdentity(std::span<std::byte const>{after.bytes});
    REQUIRE(optBeforeIdentity);
    REQUIRE(optAfterIdentity);
    CHECK(optBeforeIdentity->payloadLength == truncatedBytes);
    CHECK(optBeforeIdentity->signature == optAfterIdentity->signature);
  }

  TEST_CASE("MPEG File - keeps tag-containing audio before a real APEv2 footer")
  {
    // A TAG marker lands at physical EOF - 128 inside the last frame while a
    // real APEv2 footer follows the audio: the chain witness must run to the
    // independently parsed APE boundary, not to physical EOF.
    auto const frame = createMpegFrame(kCdRateFrameBytes, kCdRateBitrateRateByte);
    auto data = createFrameChain(frame, kChainFrameCount);
    auto const audioBytes = data.size();
    auto footer = std::vector<std::uint8_t>(32, 0);
    std::memcpy(footer.data(), "APETAGEX", 8);
    footer[8] = 0xD0;
    footer[9] = 0x07;
    footer[12] = 32;
    data.insert_range(data.end(), footer);
    std::memcpy(data.data() + data.size() - kId3v1TagBytes, "TAG", 3);

    auto const payload = readAudioPayload(data);
    CHECK(payload.offset == 0);
    CHECK(payload.bytes.size() == audioBytes);
    CHECK(std::ranges::equal(payload.bytes, asFileBytes(data).first(audioBytes)));
  }

  TEST_CASE("MPEG File - preserves up to three terminal junk bytes in a tag-containing chain")
  {
    auto const junkBytes = GENERATE(1U, 2U, 3U);
    auto const frame = createMpegFrame(kCdRateFrameBytes, kCdRateBitrateRateByte);
    auto data = createFrameChain(frame, kChainFrameCount);
    data.insert(data.end(), junkBytes, 0);
    // Keep the marker exactly at physical EOF - 128, inside the last frame.
    std::memcpy(data.data() + data.size() - kId3v1TagBytes, "TAG", 3);

    auto const payload = readAudioPayload(data);
    CAPTURE(junkBytes);
    CHECK(payload.offset == 0);
    CHECK(payload.bytes.size() == data.size());
    CHECK(std::ranges::equal(payload.bytes, asFileBytes(data)));
  }

  TEST_CASE("MPEG File - crops at the ID3v1 marker when four terminal junk bytes break the chain")
  {
    auto const frame = createMpegFrame(kCdRateFrameBytes, kCdRateBitrateRateByte);
    auto data = createFrameChain(frame, kChainFrameCount);
    data.insert(data.end(), 4, 0);
    auto const markerOffset = data.size() - kId3v1TagBytes;
    std::memcpy(data.data() + markerOffset, "TAG", 3);

    auto const payload = readAudioPayload(data);
    CHECK(payload.offset == 0);
    CHECK(payload.bytes.size() == markerOffset);
    CHECK(std::ranges::equal(payload.bytes, asFileBytes(data).first(markerOffset)));
  }

  TEST_CASE("MPEG File - does not resynchronize past a gap after an overlapping frame")
  {
    auto const frame = createMpegFrame(kCdRateFrameBytes, kCdRateBitrateRateByte);
    auto data = createFrameChain(frame, kChainFrameCount);
    data.insert(data.end(), 4, 0);
    data.insert_range(data.end(), createMpegFrame(96, 0x14));
    auto const markerOffset = data.size() - kId3v1TagBytes;
    std::memcpy(data.data() + markerOffset, "TAG", 3);

    auto const payload = readAudioPayload(data);
    CHECK(payload.offset == 0);
    CHECK(payload.bytes.size() == markerOffset);
    CHECK(std::ranges::equal(payload.bytes, asFileBytes(data).first(markerOffset)));
  }

  TEST_CASE("MPEG File - retains an ID3v1 marker in a penultimate frame before a later frame")
  {
    // 32 kbit/s at 48000 Hz gives 96-byte frames; the marker sits in frame
    // five of six, so the witness must continue past the containing frame to
    // the complete final frame instead of accepting the containing frame alone.
    constexpr std::size_t kShortFrameBytes = 96;
    auto const frame = createMpegFrame(kShortFrameBytes, 0x14);
    auto data = createFrameChain(frame, 6);
    std::memcpy(data.data() + data.size() - kId3v1TagBytes, "TAG", 3);

    auto const payload = readAudioPayload(data);
    CHECK(payload.offset == 0);
    CHECK(payload.bytes.size() == data.size());
    CHECK(std::ranges::equal(payload.bytes, asFileBytes(data)));
  }

  TEST_CASE("MPEG File - retains byte-identical tag ambiguities that complete the declared chain")
  {
    // 128 kbit/s at 48000 Hz gives 384-byte frames with zero side
    // information. Two histories produce the exact same 2304 bytes: a complete
    // sixth frame whose ancillary region holds tag-shaped data, and a
    // truncated sixth frame followed by a genuine ID3v1 tag. No byte-level
    // selector can discover which history wrote the file; the policy
    // deliberately retains the chain reading, and retitling still changes
    // the payload identity. This documents the residual ambiguity, not a
    // provenance guarantee.
    constexpr std::size_t kAmbiguousFrameBytes = 384;
    auto const frame = createMpegFrame(kAmbiguousFrameBytes, 0x94);
    auto buildAmbiguousFile = [&](std::string_view title)
    {
      auto const tag = createId3v1Tag(title);
      auto completeHistory = createFrameChain(frame, 6);
      std::ranges::copy(tag, completeHistory.end() - kId3v1TagBytes);
      auto truncatedHistory = createFrameChain(frame, 5);
      truncatedHistory.insert_range(
        truncatedHistory.end(), std::span<std::uint8_t const>{frame}.first(frame.size() - kId3v1TagBytes));
      truncatedHistory.insert_range(truncatedHistory.end(), tag);
      CHECK(completeHistory == truncatedHistory);
      return completeHistory;
    };

    auto const beforeData = buildAmbiguousFile("Before");
    auto const afterData = buildAmbiguousFile("After!");
    auto const before = readAudioPayload(beforeData);
    auto const after = readAudioPayload(afterData);

    CHECK(before.bytes.size() == 2304);
    CHECK(after.bytes.size() == 2304);
    CHECK(std::ranges::equal(before.bytes, asFileBytes(beforeData)));
    CHECK(std::ranges::equal(after.bytes, asFileBytes(afterData)));
    CHECK(before.hash != after.hash);
  }

  TEST_CASE("MPEG File - retains a tag-embedded fake frame that completes the chain at EOF")
  {
    // A 393-byte fragment borrows 24 tag bytes to finish its declared frame,
    // and the remaining tag bytes start with a fake 104-byte frame header
    // (32 kbit/s at 44100 Hz), so the apparent chain ends exactly at physical
    // EOF. Header syntax cannot distinguish ancillary audio from tag data
    // arranged as frames: the retention stays title-sensitive. This is a
    // documented limitation, not a provenance guarantee.
    auto beforeData = createTruncatedTailFile(393, "Before");
    auto afterData = createTruncatedTailFile(393, "After!");

    for (auto* data : {&beforeData, &afterData})
    {
      auto const tagOffset = data->size() - kId3v1TagBytes;
      (*data)[tagOffset + 24] = 0xFF;
      (*data)[tagOffset + 25] = 0xFB;
      (*data)[tagOffset + 26] = 0x10;
      (*data)[tagOffset + 27] = 0x00;
    }

    auto const before = readAudioPayload(beforeData);
    auto const after = readAudioPayload(afterData);

    CHECK(before.bytes.size() == 2606);
    CHECK(after.bytes.size() == 2606);
    CHECK(std::ranges::equal(before.bytes, asFileBytes(beforeData)));
    CHECK(std::ranges::equal(after.bytes, asFileBytes(afterData)));
    CHECK(before.hash != after.hash);
  }

  TEST_CASE("MPEG File - crops a trailing tag when an incompatible fake frame completes the chain at EOF")
  {
    // Each row mirrors the retained fake-frame arrangement above, but the
    // tag-embedded header that ends exactly at physical EOF changes version,
    // layer, or sampling rate and defeats the matching-parameter overlap
    // exemption.
    // The payload stays the pre-tag audio extent (5 * 417 + fragment) and
    // stays title-invariant. The compatible header in the retained case
    // above (same version, layer, and sampling rate, different bitrate)
    // remains the accepted VBR ambiguity.
    struct IncompatibleCase final
    {
      std::string_view name;
      std::size_t fragmentBytes;
      std::size_t fakeOffsetInTag;
      std::array<std::uint8_t, 4> fakeHeader;
      std::size_t expectedPayloadBytes;
    };

    auto const cases = std::to_array<IncompatibleCase>({
      // The 393-byte fragment leaves the declared frame 24 tag bytes short;
      // the fake MPEG2 Layer III header (32 kbit/s, 22050 Hz) then spans
      // 576/8 * 32000 / 22050 = 104 bytes to exactly EOF.
      {.name = "version",
       .fragmentBytes = 393,
       .fakeOffsetInTag = 24,
       .fakeHeader = {0xFF, 0xF3, 0x40, 0x00},
       .expectedPayloadBytes = 2478},
      // The fake MPEG1 Layer II header (32 kbit/s, 44100 Hz) spans
      // 1152/8 * 32000 / 44100 = 104 bytes to exactly EOF.
      {.name = "layer",
       .fragmentBytes = 393,
       .fakeOffsetInTag = 24,
       .fakeHeader = {0xFF, 0xFD, 0x10, 0x00},
       .expectedPayloadBytes = 2478},
      // The 385-byte fragment leaves the declared frame 32 tag bytes short;
      // the fake MPEG1 Layer III header (32 kbit/s, 48000 Hz) then spans
      // 144 * 32000 / 48000 = 96 bytes to exactly EOF.
      {.name = "sampling rate",
       .fragmentBytes = 385,
       .fakeOffsetInTag = 32,
       .fakeHeader = {0xFF, 0xFB, 0x14, 0x00},
       .expectedPayloadBytes = 2470},
    });

    for (auto const& incompatibleCase : cases)
    {
      CAPTURE(incompatibleCase.name);
      auto buildIncompatibleFile = [&](std::string_view title)
      {
        auto data = createTruncatedTailFile(incompatibleCase.fragmentBytes, title);
        auto const tagOffset = data.size() - kId3v1TagBytes;
        auto const frameOffset = tagOffset + incompatibleCase.fakeOffsetInTag;
        std::ranges::copy(incompatibleCase.fakeHeader, data.begin() + static_cast<std::ptrdiff_t>(frameOffset));
        auto const fakeFrame = FrameView{data.data() + frameOffset, data.size() - frameOffset};
        REQUIRE(fakeFrame.isValid());
        REQUIRE(fakeFrame.length() == fakeFrame.size());
        return data;
      };

      auto const beforeData = buildIncompatibleFile("Before");
      auto const afterData = buildIncompatibleFile("After!");
      auto const before = readAudioPayload(beforeData);
      auto const after = readAudioPayload(afterData);

      CHECK(before.offset == 0);
      CHECK(before.bytes.size() == incompatibleCase.expectedPayloadBytes);
      CHECK(after.bytes.size() == incompatibleCase.expectedPayloadBytes);
      CHECK(before.hash == after.hash);
      CHECK(std::ranges::equal(before.bytes, bytesBeforeTrailingTag(beforeData)));
      CHECK(std::ranges::equal(after.bytes, bytesBeforeTrailingTag(afterData)));
    }
  }
} // namespace ao::media::file::mpeg::test
