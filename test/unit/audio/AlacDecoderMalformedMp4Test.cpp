// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/AlacDecoderSession.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/media/mp4/TestAtoms.h"
#include <ao/audio/ChannelLayoutEvidence.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/media/mp4/Demuxer.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    constexpr std::uint32_t kMdatPayloadOffset = 8;
    constexpr std::size_t kAlacCookieCompatibleVersionOffset = 16;
    constexpr std::size_t kAlacCookieBitDepthOffset = 17;
    constexpr std::size_t kAlacCookieFrameLengthOffset = 12;
    constexpr std::size_t kAlacCookieChannelCountOffset = 21;
    constexpr std::size_t kAlacCookieSampleRateOffset = 32;

    struct AlacFixtureBytes final
    {
      std::vector<std::uint8_t> cookie;
      std::vector<std::uint8_t> firstPacket;
    };

    struct SyntheticAlacMp4Options final
    {
      std::vector<std::uint8_t> payload;
      std::uint32_t chunkOffset = kMdatPayloadOffset;
      std::uint32_t sampleSize = 0;
      std::uint32_t sampleCount = 1;
      std::uint32_t sampleDelta = 1024;
      std::uint32_t timescale = 44100;
      std::uint32_t duration = 44100;
      bool includeTiming = true;
      std::vector<std::uint8_t> siblingAtoms{};
      std::vector<std::uint8_t> tracksBefore{};
      std::vector<std::uint8_t> tracksAfter{};
    };

    std::span<std::byte const> asBytes(std::span<std::uint8_t const> data) noexcept
    {
      return {reinterpret_cast<std::byte const*>(data.data()), data.size()};
    }

    std::vector<std::uint8_t> copyBytes(std::span<std::byte const> bytes)
    {
      auto const* begin = reinterpret_cast<std::uint8_t const*>(bytes.data());
      return {begin, begin + bytes.size()};
    }

    std::uint32_t readBigEndian32(std::vector<std::uint8_t> const& bytes, std::size_t offset)
    {
      REQUIRE(offset + 4 <= bytes.size());
      return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
             (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
             (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) | static_cast<std::uint32_t>(bytes[offset + 3]);
    }

    void writeBigEndian32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
    {
      REQUIRE(offset + 4 <= bytes.size());
      bytes[offset] = static_cast<std::uint8_t>(value >> 24U);
      bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16U);
      bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8U);
      bytes[offset + 3] = static_cast<std::uint8_t>(value);
    }

    AlacFixtureBytes loadAlacFixture()
    {
      auto const path = requireAudioFixture("alac16.m4a");

      auto const fileData = readFileBytes(path);
      auto const demuxerRes = media::mp4::Demuxer::parse(asBytes(fileData), "alac");

      REQUIRE(demuxerRes);
      REQUIRE(demuxerRes->sampleCount() > 0);

      return {
        .cookie = copyBytes(demuxerRes->magicCookie()),
        .firstPacket = copyBytes(demuxerRes->samplePayload(0)),
      };
    }

    std::vector<std::uint8_t> makeSyntheticAlacMp4(std::vector<std::uint8_t> const& cookie,
                                                   SyntheticAlacMp4Options options)
    {
      if (options.sampleSize == 0)
      {
        options.sampleSize = static_cast<std::uint32_t>(options.payload.size());
      }

      auto data = std::vector<std::uint8_t>{};
      ao::test::mp4::addAtom(data, "mdat", options.payload);

      auto extensions = cookie;
      extensions.insert(extensions.end(), options.siblingAtoms.begin(), options.siblingAtoms.end());
      auto sampleEntry = ao::test::mp4::makeAudioSampleEntryAtom("alac", extensions, options.timescale);

      if (cookie.size() > kAlacCookieChannelCountOffset)
      {
        // AudioSampleEntry's big-endian channelCount occupies bytes 24/25.
        sampleEntry[25] = cookie[kAlacCookieChannelCountOffset];
      }

      auto const stsd = ao::test::mp4::makeStsdAtomFromSampleEntry(sampleEntry);
      auto audioTrack = std::vector<std::uint8_t>{};

      if (options.includeTiming)
      {
        auto const stbl =
          ao::test::mp4::makeSampleTableAtom(stsd, options.sampleSize, options.sampleDelta, options.chunkOffset);
        audioTrack = ao::test::mp4::makeTrackAtom("soun", stbl, options.timescale, options.duration);
      }
      else
      {
        auto stblBody = std::vector<std::uint8_t>{};
        auto const stsz = ao::test::mp4::makeStszAtom(options.sampleSize, options.sampleCount);
        auto const stsc = ao::test::mp4::makeStscAtom(options.sampleCount);
        auto const stco = ao::test::mp4::makeStcoAtom(options.chunkOffset);
        stblBody.insert(stblBody.end(), stsd.begin(), stsd.end());
        stblBody.insert(stblBody.end(), stsz.begin(), stsz.end());
        stblBody.insert(stblBody.end(), stsc.begin(), stsc.end());
        stblBody.insert(stblBody.end(), stco.begin(), stco.end());

        auto const stbl = ao::test::mp4::makeAtom("stbl", stblBody);
        audioTrack = ao::test::mp4::makeTrackAtom("soun", stbl, options.timescale, options.duration);
      }

      auto tracks = std::move(options.tracksBefore);
      tracks.insert(tracks.end(), audioTrack.begin(), audioTrack.end());
      tracks.insert(tracks.end(), options.tracksAfter.begin(), options.tracksAfter.end());
      auto const moov = ao::test::mp4::makeAtom("moov", tracks);
      data.insert(data.end(), moov.begin(), moov.end());
      return data;
    }

    std::vector<std::uint8_t> literalAlacCookie(std::uint8_t channels,
                                                std::vector<std::uint8_t> const& optionalAtoms = {})
    {
      // Compact atom, flags, then ALACSpecificConfig: 16 frames, 16 bits,
      // pb/mb/kb 40/10/14, maxRun 255, 48 kHz. No encoder is involved.
      auto cookie =
        std::vector<std::uint8_t>{0,  0,  0,  36,       'a', 'l', 'a', 'c', 0, 0,  0, 0, 0, 0, 0, 16, 0,    16,
                                  40, 10, 14, channels, 0,   255, 0,   0,   0, 72, 0, 0, 0, 0, 0, 0,  0xBB, 0x80};
      cookie.insert(cookie.end(), optionalAtoms.begin(), optionalAtoms.end());
      writeBigEndian32(cookie, 0, static_cast<std::uint32_t>(cookie.size()));
      return cookie;
    }

    std::vector<std::uint8_t> literalAlacPacket(std::uint8_t channels)
    {
      // Reference ALAC escaped SCE/CPE grammar, partial frame of 16 samples.
      // Samples start at bit 55: 0x5152 (mono), 0x5152/0x6364 (stereo).
      auto packet = std::vector<std::uint8_t>{
        static_cast<std::uint8_t>(channels == 1 ? 0x00 : 0x20), 0x00, 0x12, 0x00, 0x00, 0x00, 0x20};

      for (std::uint32_t frame = 0; frame < 16; ++frame)
      {
        packet.insert(packet.end(), {0xA2, 0xA4});

        if (channels == 2)
        {
          packet.insert(packet.end(), {0xC6, 0xC8});
        }
      }

      packet.back() |= 1U; // ID_END starts in the last sample byte's low bit.
      packet.push_back(0xC0);
      return packet;
    }

    std::vector<std::uint8_t> literalAlacPcm(std::uint8_t channels)
    {
      auto pcm = std::vector<std::uint8_t>{};

      for (std::uint32_t frame = 0; frame < 16; ++frame)
      {
        pcm.insert(pcm.end(), {0x52, 0x51});

        if (channels == 2)
        {
          pcm.insert(pcm.end(), {0x64, 0x63});
        }
      }

      return pcm;
    }

    std::vector<std::uint8_t> alacChan(std::uint32_t tag, std::uint32_t bitmap = 0, std::uint32_t descriptionCount = 0)
    {
      auto body = std::vector<std::uint8_t>{};
      ao::test::mp4::appendBe32(body, 0);
      ao::test::mp4::appendBe32(body, tag);
      ao::test::mp4::appendBe32(body, bitmap);
      ao::test::mp4::appendBe32(body, descriptionCount);
      return ao::test::mp4::makeAtom("chan", body);
    }

    void checkLiteralAlac(std::vector<std::uint8_t> const& mp4Data, std::uint8_t channels, bool identified)
    {
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      auto decoderPtr = ao::test::requireValue(AlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
      auto const info = decoderPtr->streamInfo(); // Layout must be ready before PCM.
      std::uint32_t const identifiedMask = channels == 1 ? 0x4U : 0x3U;
      std::uint32_t const mask = identified ? identifiedMask : 0U;
      CHECK(info.sourceFormat.channels == channels);
      CHECK(info.sourceFormat.sampleRate == 48000U);
      CHECK(info.sourceFormat.precisionBits == 16U);
      CHECK(info.sourceFormat.speakerMask == mask);
      CHECK(info.outputFormat.channels == channels);
      CHECK(info.outputFormat.speakerMask == mask);
      CHECK(info.sourceChannelLayout.origin ==
            (identified ? ChannelLayoutOrigin::Identified : ChannelLayoutOrigin::Unknown));
      auto const blockRes = decoderPtr->readNextBlock();
      REQUIRE(blockRes);
      CHECK(blockRes->frames == 16U);
      CHECK(blockRes->firstFrameIndex == 0U);
      CHECK(blockRes->endOfStream);
      auto const expected = literalAlacPcm(channels);
      CHECK(std::ranges::equal(blockRes->bytes, asBytes(expected)));
      CHECK(decoderPtr->streamInfo() == info);
      auto const endRes = decoderPtr->readNextBlock();
      REQUIRE(endRes);
      CHECK(endRes->frames == 0U);
      CHECK(endRes->bytes.empty());
      CHECK(endRes->endOfStream);
      REQUIRE(decoderPtr->seek(std::chrono::milliseconds{0}));
      auto const resetRes = decoderPtr->readNextBlock();
      REQUIRE(resetRes);
      CHECK(resetRes->frames == 16U);
      CHECK(resetRes->firstFrameIndex == 0U);
      CHECK(std::ranges::equal(resetRes->bytes, asBytes(expected)));
      CHECK(decoderPtr->streamInfo() == info);
    }
  } // namespace

  TEST_CASE("AlacDecoderSession - mono and stereo defaults require every relevant chan to be canonical",
            "[audio][unit][alac][mp4]")
  {
    auto const channels = GENERATE(std::uint8_t{1}, std::uint8_t{2});
    auto const matchingTag = channels == 1 ? 0x0064'0001U : 0x0065'0002U;
    auto const canonical = alacChan(matchingTag);
    auto const mismatch = alacChan(channels == 1 ? 0x0065'0002U : 0x0064'0001U);
    auto const other = alacChan(0x0066'0002U);

    struct LayoutCase final
    {
      std::string_view name;
      std::vector<std::uint8_t> cookieAtoms;
      std::vector<std::uint8_t> siblingAtoms;
      bool identified;
    };

    auto descriptions = alacChan(0, 0, channels);

    // Even bounded C or L/R descriptions are outside the accepted ALAC tags.
    for (std::uint32_t index = 0; index < channels; ++index)
    {
      ao::test::mp4::appendBe32(descriptions, channels == 1 ? 3U : index + 1U);
      ao::test::mp4::appendBe32(descriptions, 0); // Flags
      ao::test::mp4::appendBe32(descriptions, 0); // X
      ao::test::mp4::appendBe32(descriptions, 0); // Y
      ao::test::mp4::appendBe32(descriptions, 0); // Z
    }

    writeBigEndian32(descriptions, 0, static_cast<std::uint32_t>(descriptions.size()));
    auto namedDescriptions = descriptions;
    writeBigEndian32(namedDescriptions, 12, matchingTag);
    auto shortHeader = canonical;
    shortHeader.pop_back();
    writeBigEndian32(shortHeader, 0, static_cast<std::uint32_t>(shortHeader.size()));
    auto duplicateConflict = canonical;
    duplicateConflict.insert(duplicateConflict.end(), mismatch.begin(), mismatch.end());
    auto duplicateCanonical = canonical;
    duplicateCanonical.insert(duplicateCanonical.end(), canonical.begin(), canonical.end());
    auto extraBytes = canonical;
    extraBytes.push_back(0);
    writeBigEndian32(extraBytes, 0, static_cast<std::uint32_t>(extraBytes.size()));
    auto nonzeroFlags = canonical;
    writeBigEndian32(nonzeroFlags, 8, 1);
    auto const extendedCanonical = ao::test::mp4::makeExtendedFromCompactAtom(canonical);
    auto const cases = std::vector<LayoutCase>{
      {"absent", {}, {}, true},
      {"cookie canonical", canonical, {}, true},
      {"sibling canonical", {}, canonical, true},
      {"both canonical", canonical, canonical, true},
      {"extended cookie canonical", extendedCanonical, {}, true},
      {"extended sibling canonical", {}, extendedCanonical, true},
      {"repeated canonical participants", duplicateCanonical, duplicateCanonical, true},
      {"explicit zero tag is not absence", alacChan(0), {}, false},
      {"cookie other", other, {}, false},
      {"sibling other", {}, other, false},
      {"cookie mismatched count", mismatch, {}, false},
      {"sibling mismatched count", {}, mismatch, false},
      {"canonical cookie cannot hide other sibling", canonical, other, false},
      {"canonical sibling cannot hide other cookie", other, canonical, false},
      {"conflicting cookie participants", duplicateConflict, {}, false},
      {"conflicting sibling participants", {}, duplicateConflict, false},
      {"bitmap is not interpreted", alacChan(0x0001'0000U, channels == 1 ? 4U : 3U), {}, false},
      {"descriptions are not interpreted", {}, descriptions, false},
      {"short bounded chan", ao::test::mp4::makeAtom("chan", {0}), {}, false},
      {"matching tag with incomplete fixed header", shortHeader, shortHeader, false},
      {"named tag ignores trailing bytes", extraBytes, extraBytes, true},
      {"named tag ignores flags", nonzeroFlags, nonzeroFlags, true},
      {"named tag ignores bitmap", alacChan(matchingTag, 4), alacChan(matchingTag, 4), true},
      {"named tag ignores descriptions", namedDescriptions, namedDescriptions, true},
      {"named tag ignores description count", alacChan(matchingTag, 0, channels), {}, true},
    };

    for (auto const& layout : cases)
    {
      CAPTURE(channels, layout.name);
      auto const mp4Data = makeSyntheticAlacMp4(literalAlacCookie(channels, layout.cookieAtoms),
                                                {.payload = literalAlacPacket(channels),
                                                 .sampleDelta = 16,
                                                 .timescale = 48000,
                                                 .duration = 16,
                                                 .siblingAtoms = layout.siblingAtoms});
      checkLiteralAlac(mp4Data, channels, layout.identified);
    }
  }

  TEST_CASE("AlacDecoderSession - unrelated tracks do not suppress selected ALAC defaults", "[audio][unit][alac][mp4]")
  {
    auto const channels = GENERATE(std::uint8_t{1}, std::uint8_t{2});
    auto otherExtensions = literalAlacCookie(channels);
    auto const other = alacChan(0);
    otherExtensions.insert(otherExtensions.end(), other.begin(), other.end());
    auto const stsd = ao::test::mp4::makeStsdAtom("alac", otherExtensions);
    auto const stbl = ao::test::mp4::makeSampleTableAtom(stsd);
    auto const mp4Data = makeSyntheticAlacMp4(literalAlacCookie(channels),
                                              {.payload = literalAlacPacket(channels),
                                               .sampleDelta = 16,
                                               .timescale = 48000,
                                               .duration = 16,
                                               .tracksBefore = ao::test::mp4::makeTrackAtom("vide", stbl),
                                               .tracksAfter = ao::test::mp4::makeTrackAtom("soun", stbl)});
    checkLiteralAlac(mp4Data, channels, true);
  }

  TEST_CASE("AlacDecoderSession - structural chan failures remain initialization errors", "[audio][unit][alac][mp4]")
  {
    bool const inCookie = GENERATE(false, true);
    auto const malformed = GENERATE(std::vector<std::uint8_t>{0, 0, 0},
                                    std::vector<std::uint8_t>{0, 0, 0, 32, 'c', 'h', 'a', 'n'},
                                    std::vector<std::uint8_t>{0, 0, 0, 0, 'c', 'h', 'a', 'n'});
    CAPTURE(inCookie, malformed);
    auto const mp4Data = makeSyntheticAlacMp4(
      literalAlacCookie(2, inCookie ? malformed : std::vector<std::uint8_t>{}),
      {.payload = literalAlacPacket(2), .siblingAtoms = inCookie ? std::vector<std::uint8_t>{} : malformed});
    auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
    auto const decoderRes = AlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le);
    REQUIRE_FALSE(decoderRes);
    CHECK(decoderRes.error().code == Error::Code::InitFailed);
  }

  TEST_CASE("AlacDecoderSession - unknown chan does not hide invalid mandatory configuration",
            "[audio][unit][alac][mp4]")
  {
    auto cookie = literalAlacCookie(2, alacChan(0));
    cookie[kAlacCookieBitDepthOffset] = 0;
    auto const mp4Data = makeSyntheticAlacMp4(cookie, {.payload = literalAlacPacket(2), .siblingAtoms = alacChan(0)});
    auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
    auto const decoderRes = AlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le);
    REQUIRE_FALSE(decoderRes);
    CHECK(decoderRes.error().code == Error::Code::InitFailed);
  }

  TEST_CASE("AlacDecoderSession - multichannel layout interpretation remains unknown", "[audio][unit][alac][mp4]")
  {
    auto const channels =
      GENERATE(std::uint8_t{3}, std::uint8_t{4}, std::uint8_t{5}, std::uint8_t{6}, std::uint8_t{7}, std::uint8_t{8});
    auto const mp4Data = makeSyntheticAlacMp4(literalAlacCookie(channels), {.payload = literalAlacPacket(2)});
    auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
    auto decoderPtr = ao::test::requireValue(AlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
    auto const info = decoderPtr->streamInfo();
    CHECK(info.sourceFormat.channels == channels);
    CHECK(info.sourceFormat.speakerMask == 0U);
    CHECK(info.outputFormat.speakerMask == 0U);
    CHECK(info.sourceChannelLayout.origin == ChannelLayoutOrigin::Unknown);
  }

  TEST_CASE("AlacDecoderSession - rejects malformed MP4 sample data", "[audio][unit][alac][mp4]")
  {
    auto const fixture = loadAlacFixture();

    SECTION("Sample offset points outside the file")
    {
      auto const mp4Data =
        makeSyntheticAlacMp4(fixture.cookie, {.payload = {0x01, 0x02, 0x03, 0x04}, .chunkOffset = 0x00FF'FFFFU});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      auto decoderPtr = ao::test::requireValue(AlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
      auto& decoder = *decoderPtr;
      CHECK(!decoder.readNextBlock());
    }

    SECTION("Corrupt packet fails direct decode")
    {
      auto const mp4Data = makeSyntheticAlacMp4(fixture.cookie, {.payload = std::vector<std::uint8_t>(32, 0xA5)});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      auto decoderPtr = ao::test::requireValue(AlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
      auto& decoder = *decoderPtr;
      CHECK(!decoder.readNextBlock());
    }

    SECTION("Corrupt packet fails padded decode")
    {
      auto const mp4Data = makeSyntheticAlacMp4(fixture.cookie, {.payload = std::vector<std::uint8_t>(32, 0xA5)});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      auto decoderPtr = ao::test::requireValue(AlacDecoderSession::open(temp.path, SampleEncoding::Signed32Le));
      auto& decoder = *decoderPtr;
      CHECK(!decoder.readNextBlock());
    }
  }

  TEST_CASE("AlacDecoderSession - rejects malformed ALAC config", "[audio][unit][alac][mp4]")
  {
    auto const fixture = loadAlacFixture();

    SECTION("Unsupported compatible version")
    {
      auto cookie = fixture.cookie;
      REQUIRE(cookie.size() > kAlacCookieCompatibleVersionOffset);
      cookie[kAlacCookieCompatibleVersionOffset] = 1;

      auto const mp4Data = makeSyntheticAlacMp4(cookie, {.payload = {0, 0, 0, 0}});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      CHECK(!AlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
    }

    SECTION("Zero bit depth")
    {
      auto cookie = fixture.cookie;
      REQUIRE(cookie.size() > kAlacCookieBitDepthOffset);
      cookie[kAlacCookieBitDepthOffset] = 0;

      auto const mp4Data = makeSyntheticAlacMp4(cookie, {.payload = {0, 0, 0, 0}});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      CHECK(!AlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
    }

    SECTION("Oversized frame length is rejected before decoder allocation")
    {
      auto cookie = fixture.cookie;
      writeBigEndian32(cookie, kAlacCookieFrameLengthOffset, 16385);

      auto const mp4Data = makeSyntheticAlacMp4(cookie, {.payload = {0, 0, 0, 0}});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      CHECK(!AlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
    }

    SECTION("Unsupported channel count is rejected before decoder allocation")
    {
      auto cookie = fixture.cookie;
      REQUIRE(cookie.size() > kAlacCookieChannelCountOffset);
      cookie[kAlacCookieChannelCountOffset] = 9;

      auto const mp4Data = makeSyntheticAlacMp4(cookie, {.payload = {0, 0, 0, 0}});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      CHECK(!AlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
    }

    SECTION("Unsupported sample rate is rejected before decoder allocation")
    {
      auto cookie = fixture.cookie;
      writeBigEndian32(cookie, kAlacCookieSampleRateOffset, 384001);

      auto const mp4Data = makeSyntheticAlacMp4(cookie, {.payload = {0, 0, 0, 0}});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      CHECK(!AlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
    }
  }

  TEST_CASE("AlacDecoderSession - MP4 timing fallbacks", "[audio][unit][alac][mp4]")
  {
    auto const fixture = loadAlacFixture();

    SECTION("Uses source sample rate when mdhd timescale is zero")
    {
      auto const mp4Data =
        makeSyntheticAlacMp4(fixture.cookie, {.payload = fixture.firstPacket, .timescale = 0, .duration = 44100});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      auto decoderPtr = ao::test::requireValue(AlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
      auto& decoder = *decoderPtr;

      auto const info = decoder.streamInfo();
      CHECK(info.sourceFormat.sampleRate == 44100);
      CHECK(info.duration == std::chrono::seconds{1});
    }

    SECTION("Uses frame length when sample timing is absent")
    {
      auto payload = fixture.firstPacket;
      payload.insert(payload.end(), fixture.firstPacket.begin(), fixture.firstPacket.end());
      auto const frameLength = readBigEndian32(fixture.cookie, kAlacCookieFrameLengthOffset);
      auto const mp4Data = makeSyntheticAlacMp4(fixture.cookie,
                                                {.payload = std::move(payload),
                                                 .sampleSize = static_cast<std::uint32_t>(fixture.firstPacket.size()),
                                                 .sampleCount = 2,
                                                 .includeTiming = false});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      auto decoderPtr = ao::test::requireValue(AlacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
      auto& decoder = *decoderPtr;

      auto const firstBlockRes = decoder.readNextBlock();
      REQUIRE(firstBlockRes);
      REQUIRE(firstBlockRes->frames > 0);
      CHECK(firstBlockRes->firstFrameIndex == 0);

      auto const secondBlockRes = decoder.readNextBlock();
      REQUIRE(secondBlockRes);
      CHECK(secondBlockRes->frames > 0);
      CHECK(secondBlockRes->firstFrameIndex == frameLength);
    }
  }
} // namespace ao::audio::test
