// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/media/mp4/TestAtoms.h"
#include <ao/audio/AlacDecoderSession.h>
#include <ao/audio/Format.h>
#include <ao/media/mp4/Demuxer.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
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
      std::uint32_t timescale = 44100;
      std::uint32_t duration = 44100;
      bool includeTiming = true;
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
      auto const path = std::filesystem::path{TAG_TEST_DATA_DIR} / "alac16.m4a";

      if (!std::filesystem::exists(path))
      {
        SKIP("Required audio fixture missing: " << path);
      }

      auto const fileData = readFileBytes(path);
      auto demuxer = media::mp4::Demuxer{asBytes(fileData)};

      REQUIRE(demuxer.parseTrack("alac"));
      REQUIRE(demuxer.sampleCount() > 0);

      return {
        .cookie = copyBytes(demuxer.magicCookie()),
        .firstPacket = copyBytes(demuxer.samplePayload(0)),
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

      auto audioTrack = std::vector<std::uint8_t>{};

      if (options.includeTiming)
      {
        audioTrack = ao::test::mp4::makeCompleteAudioTrackAtom(
          "alac", cookie, options.timescale, options.duration, options.sampleSize, 1024, options.chunkOffset);
      }
      else
      {
        auto stblBody = std::vector<std::uint8_t>{};
        auto const stsd = ao::test::mp4::makeStsdAtom("alac", cookie, options.timescale);
        auto const stsz = ao::test::mp4::makeStszAtom(options.sampleSize);
        auto const stsc = ao::test::mp4::makeStscAtom();
        auto const stco = ao::test::mp4::makeStcoAtom(options.chunkOffset);
        stblBody.insert(stblBody.end(), stsd.begin(), stsd.end());
        stblBody.insert(stblBody.end(), stsz.begin(), stsz.end());
        stblBody.insert(stblBody.end(), stsc.begin(), stsc.end());
        stblBody.insert(stblBody.end(), stco.begin(), stco.end());

        auto const stbl = ao::test::mp4::makeAtom("stbl", stblBody);
        audioTrack = ao::test::mp4::makeTrackAtom("soun", stbl, options.timescale, options.duration);
      }

      auto const moov = ao::test::mp4::makeAtom("moov", audioTrack);
      data.insert(data.end(), moov.begin(), moov.end());
      return data;
    }
  } // namespace

  TEST_CASE("AlacDecoderSession - rejects malformed MP4 sample data", "[audio][unit][alac][mp4]")
  {
    auto const fixture = loadAlacFixture();

    SECTION("Sample offset points outside the file")
    {
      auto const mp4Data =
        makeSyntheticAlacMp4(fixture.cookie, {.payload = {0x01, 0x02, 0x03, 0x04}, .chunkOffset = 0x00FF'FFFFU});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      auto decoder = AlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      REQUIRE(decoder.open(temp.path));
      CHECK(!decoder.readNextBlock());
    }

    SECTION("Corrupt packet fails direct decode")
    {
      auto const mp4Data = makeSyntheticAlacMp4(fixture.cookie, {.payload = std::vector<std::uint8_t>(32, 0xA5)});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      auto decoder = AlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      REQUIRE(decoder.open(temp.path));
      CHECK(!decoder.readNextBlock());
    }

    SECTION("Corrupt packet fails padded decode")
    {
      auto const mp4Data = makeSyntheticAlacMp4(fixture.cookie, {.payload = std::vector<std::uint8_t>(32, 0xA5)});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      auto decoder = AlacDecoderSession{Format{.bitDepth = 32, .isInterleaved = true}};

      REQUIRE(decoder.open(temp.path));
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
      auto decoder = AlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      CHECK(!decoder.open(temp.path));
    }

    SECTION("Zero bit depth")
    {
      auto cookie = fixture.cookie;
      REQUIRE(cookie.size() > kAlacCookieBitDepthOffset);
      cookie[kAlacCookieBitDepthOffset] = 0;

      auto const mp4Data = makeSyntheticAlacMp4(cookie, {.payload = {0, 0, 0, 0}});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      auto decoder = AlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      CHECK(!decoder.open(temp.path));
    }

    SECTION("Oversized frame length is rejected before decoder allocation")
    {
      auto cookie = fixture.cookie;
      writeBigEndian32(cookie, kAlacCookieFrameLengthOffset, 16385);

      auto const mp4Data = makeSyntheticAlacMp4(cookie, {.payload = {0, 0, 0, 0}});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      auto decoder = AlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      CHECK(!decoder.open(temp.path));
    }

    SECTION("Unsupported channel count is rejected before decoder allocation")
    {
      auto cookie = fixture.cookie;
      REQUIRE(cookie.size() > kAlacCookieChannelCountOffset);
      cookie[kAlacCookieChannelCountOffset] = 9;

      auto const mp4Data = makeSyntheticAlacMp4(cookie, {.payload = {0, 0, 0, 0}});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      auto decoder = AlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      CHECK(!decoder.open(temp.path));
    }

    SECTION("Unsupported sample rate is rejected before decoder allocation")
    {
      auto cookie = fixture.cookie;
      writeBigEndian32(cookie, kAlacCookieSampleRateOffset, 384001);

      auto const mp4Data = makeSyntheticAlacMp4(cookie, {.payload = {0, 0, 0, 0}});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      auto decoder = AlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      CHECK(!decoder.open(temp.path));
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
      auto decoder = AlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      REQUIRE(decoder.open(temp.path));

      auto const info = decoder.streamInfo();
      CHECK(info.sourceFormat.sampleRate == 44100);
      CHECK(info.duration == std::chrono::seconds{1});
    }

    SECTION("Uses frame length when sample timing is absent")
    {
      auto const mp4Data =
        makeSyntheticAlacMp4(fixture.cookie, {.payload = fixture.firstPacket, .includeTiming = false});
      auto const temp = ao::test::TempFile{mp4Data, ".m4a"};
      auto decoder = AlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      REQUIRE(decoder.open(temp.path));

      auto const block = decoder.readNextBlock();
      REQUIRE(block);
      CHECK(block->firstFrameIndex == 0);
      CHECK(block->frames > 0);
    }
  }
} // namespace ao::audio::test
