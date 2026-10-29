// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/AacDecoderSession.h"
#include "lib/audio/DecoderFactory.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include "test/media/mp4/TestAtoms.h"
#include <ao/Error.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/media/mp4/Demuxer.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    constexpr std::uint32_t kMdatPayloadOffset = 8;

    struct SyntheticAacMp4Options final
    {
      std::vector<std::uint8_t> asc = {0x12, 0x10};
      std::vector<std::uint8_t> payload;
      std::uint32_t chunkOffset = kMdatPayloadOffset;
      std::optional<std::uint32_t> optSampleSize = std::nullopt;
      std::uint32_t sampleCount = 1;
      bool includeAsc = true;
    };

    std::vector<std::uint8_t> makeSyntheticAacMp4(SyntheticAacMp4Options options)
    {
      auto const sampleSize = options.optSampleSize.value_or(static_cast<std::uint32_t>(options.payload.size()));

      auto data = std::vector<std::uint8_t>{};
      ao::test::mp4::addAtom(data, "mdat", options.payload);

      auto esdsBody = std::vector<std::uint8_t>{0, 0, 0, 0};

      if (options.includeAsc)
      {
        esdsBody.push_back(0x05);
        esdsBody.push_back(static_cast<std::uint8_t>(options.asc.size()));
        esdsBody.insert(esdsBody.end(), options.asc.begin(), options.asc.end());
      }

      auto const esds = ao::test::mp4::makeAtom("esds", esdsBody);
      auto stblBody = ao::test::mp4::makeStsdAtom("mp4a", esds);
      auto stszBody = std::vector<std::uint8_t>{};
      ao::test::mp4::appendBe32(stszBody, 0);
      ao::test::mp4::appendBe32(stszBody, sampleSize);
      ao::test::mp4::appendBe32(stszBody, options.sampleCount);

      // A zero stsz default size requires one explicit size entry per sample.
      if (sampleSize == 0)
      {
        for (std::uint32_t sample = 0; sample < options.sampleCount; ++sample)
        {
          ao::test::mp4::appendBe32(stszBody, 0);
        }
      }

      ao::test::mp4::addAtom(stblBody, "stsz", stszBody);

      if (options.sampleCount == 0)
      {
        auto const emptyEntries = std::vector<std::uint8_t>(8, 0);
        ao::test::mp4::addAtom(stblBody, "stts", emptyEntries);
        ao::test::mp4::addAtom(stblBody, "stsc", emptyEntries);
        ao::test::mp4::addAtom(stblBody, "stco", emptyEntries);
      }
      else
      {
        stblBody.append_range(ao::test::mp4::makeSttsAtom(options.sampleCount, 1024));
        stblBody.append_range(ao::test::mp4::makeStscAtom());
        stblBody.append_range(ao::test::mp4::makeStcoAtom(options.chunkOffset));
      }

      auto const track = ao::test::mp4::makeTrackAtom("soun", ao::test::mp4::makeAtom("stbl", stblBody));
      auto const moov = ao::test::mp4::makeAtom("moov", track);
      data.insert(data.end(), moov.begin(), moov.end());
      return data;
    }
  } // namespace

  // Private priming decodes the first packet during open, so malformed first
  // sample payloads now fail open() with DecodeFailed instead of the first read.
  TEST_CASE("AacDecoderSession - rejects malformed MP4 sample data")
  {
    SECTION("Sample offset outside the file fails open")
    {
      auto const data = makeSyntheticAacMp4({
        .payload = {1, 2, 3, 4},
        .chunkOffset = 0x00FF'FFFFU,
      });
      auto const temp = ao::test::TempFile{data, ".m4a"};
      auto const openRes = AacDecoderSession::open(temp.path, SampleEncoding::Signed16Le);
      REQUIRE_FALSE(openRes);
      CHECK(openRes.error().code == Error::Code::DecodeFailed);
    }

    SECTION("Corrupt first packet fails direct decode at open")
    {
      auto const data = makeSyntheticAacMp4({.payload = std::vector<std::uint8_t>(32, 0xA5)});
      auto const temp = ao::test::TempFile{data, ".m4a"};
      auto const openRes = AacDecoderSession::open(temp.path, SampleEncoding::Signed16Le);
      REQUIRE_FALSE(openRes);
      CHECK(openRes.error().code == Error::Code::DecodeFailed);
    }

    SECTION("Corrupt first packet fails padded decode at open")
    {
      auto const data = makeSyntheticAacMp4({.payload = std::vector<std::uint8_t>(32, 0xA5)});
      auto const temp = ao::test::TempFile{data, ".m4a"};
      auto const openRes = AacDecoderSession::open(temp.path, SampleEncoding::Signed32Le);
      REQUIRE_FALSE(openRes);
      CHECK(openRes.error().code == Error::Code::DecodeFailed);
    }
  }

  TEST_CASE("AacDecoderSession - zero-sized samples fail priming while zero sample counts fail admission")
  {
    auto const fileData = readFileBytes(requireAudioFixture("aac-layout/stereo.m4a"));
    auto const fixtureDemuxerRes =
      media::mp4::Demuxer::parse(utility::bytes::view(std::span<std::uint8_t const>{fileData}), "mp4a");
    REQUIRE(fixtureDemuxerRes);
    REQUIRE_FALSE(fixtureDemuxerRes->magicCookie().empty());
    auto asc = std::vector<std::uint8_t>{};

    for (auto const byte : fixtureDemuxerRes->magicCookie())
    {
      asc.push_back(std::to_integer<std::uint8_t>(byte));
    }

    SECTION("One explicitly zero-sized first sample passes demux and fails direct and factory priming")
    {
      auto const data = makeSyntheticAacMp4({.asc = asc, .payload = {0xA5}, .optSampleSize = 0});
      auto const demuxerRes =
        media::mp4::Demuxer::parse(utility::bytes::view(std::span<std::uint8_t const>{data}), "mp4a");
      REQUIRE(demuxerRes);
      CHECK(demuxerRes->sampleCount() == 1);
      CHECK(demuxerRes->sampleInfo(0).size == 0);
      CHECK(demuxerRes->sampleInfo(0).offset == kMdatPayloadOffset);
      CHECK(demuxerRes->samplePayload(0).empty());

      auto const temp = ao::test::TempFile{data, ".m4a"};
      auto const concreteRes = AacDecoderSession::open(temp.path, SampleEncoding::Signed16Le);
      REQUIRE_FALSE(concreteRes);
      CHECK(concreteRes.error().code == Error::Code::DecodeFailed);
      CHECK(concreteRes.error().message == "AAC sample payload is empty or inaccessible");

      auto const factoryRes = openDecoderSession(temp.path, SampleEncoding::Signed16Le);
      REQUIRE_FALSE(factoryRes);
      CHECK(factoryRes.error().code == Error::Code::DecodeFailed);
      CHECK(factoryRes.error().message == "AAC sample payload is empty or inaccessible");
    }

    SECTION("Zero sample count is refused before decoder priming")
    {
      auto const data = makeSyntheticAacMp4({.asc = asc, .payload = {0xA5}, .sampleCount = 0});
      auto const demuxerRes =
        media::mp4::Demuxer::parse(utility::bytes::view(std::span<std::uint8_t const>{data}), "mp4a");
      REQUIRE_FALSE(demuxerRes);
      CHECK(demuxerRes.error().code == Error::Code::FormatRejected);

      auto const temp = ao::test::TempFile{data, ".m4a"};
      auto const concreteRes = AacDecoderSession::open(temp.path, SampleEncoding::Signed16Le);
      REQUIRE_FALSE(concreteRes);
      CHECK(concreteRes.error().code == Error::Code::InitFailed);

      auto const factoryRes = openDecoderSession(temp.path, SampleEncoding::Signed16Le);
      REQUIRE_FALSE(factoryRes);
      CHECK(factoryRes.error().code == Error::Code::InitFailed);
    }
  }

  TEST_CASE("AacDecoderSession - rejects malformed AudioSpecificConfig")
  {
    SECTION("Missing decoder-specific descriptor")
    {
      auto const data = makeSyntheticAacMp4({.payload = {1, 2, 3, 4}, .includeAsc = false});
      auto const temp = ao::test::TempFile{data, ".m4a"};
      CHECK(!AacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
    }

    SECTION("Truncated config")
    {
      auto const data = makeSyntheticAacMp4({.asc = {0x12}, .payload = {1, 2, 3, 4}});
      auto const temp = ao::test::TempFile{data, ".m4a"};
      CHECK(!AacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
    }
  }

  TEST_CASE("AacDecoderSession - required failures and the encoding preflight precede first packet priming")
  {
    SECTION("Missing decoder-specific descriptor wins over an invalid requested encoding")
    {
      auto const data = makeSyntheticAacMp4({.payload = {1, 2, 3, 4}, .includeAsc = false});
      auto const temp = ao::test::TempFile{data, ".m4a"};

      auto const concreteRes = AacDecoderSession::open(temp.path, SampleEncoding::Unknown);
      REQUIRE_FALSE(concreteRes);
      CHECK(concreteRes.error().code == Error::Code::InitFailed);

      auto const factoryRes = openDecoderSession(temp.path, SampleEncoding::Unknown);
      REQUIRE_FALSE(factoryRes);
      CHECK(factoryRes.error().code == Error::Code::InitFailed);
    }

    SECTION("Truncated config wins over an invalid requested encoding")
    {
      auto const data = makeSyntheticAacMp4({.asc = {0x12}, .payload = {1, 2, 3, 4}});
      auto const temp = ao::test::TempFile{data, ".m4a"};

      auto const concreteRes = AacDecoderSession::open(temp.path, SampleEncoding::Unknown);
      REQUIRE_FALSE(concreteRes);
      CHECK(concreteRes.error().code == Error::Code::InitFailed);

      auto const factoryRes = openDecoderSession(temp.path, SampleEncoding::Unknown);
      REQUIRE_FALSE(factoryRes);
      CHECK(factoryRes.error().code == Error::Code::InitFailed);
    }

    SECTION("Invalid requested encoding wins over a corrupt first payload")
    {
      auto const data = makeSyntheticAacMp4({.payload = std::vector<std::uint8_t>(32, 0xA5)});
      auto const temp = ao::test::TempFile{data, ".m4a"};

      auto const concreteRes = AacDecoderSession::open(temp.path, SampleEncoding::Unknown);
      REQUIRE_FALSE(concreteRes);
      CHECK(concreteRes.error().code == Error::Code::NotSupported);

      auto const factoryRes = openDecoderSession(temp.path, SampleEncoding::Unknown);
      REQUIRE_FALSE(factoryRes);
      CHECK(factoryRes.error().code == Error::Code::NotSupported);
    }
  }

  TEST_CASE("AacDecoderSession - corrupt sample after a valid first packet fails at read time")
  {
    auto fileData = readFileBytes(requireAudioFixture("aac-layout/stereo.m4a"));
    auto const demuxerRes =
      media::mp4::Demuxer::parse(utility::bytes::view(std::span<std::uint8_t const>{fileData}), "mp4a");
    REQUIRE(demuxerRes);
    REQUIRE(demuxerRes->sampleCount() > 1);

    // Corrupt only the second sample payload through the demuxer's sample
    // table; the container and sample tables stay intact.
    auto const sample = demuxerRes->sampleInfo(1);
    REQUIRE(sample.size > 0);
    REQUIRE(sample.offset + sample.size <= fileData.size());
    std::fill(fileData.begin() + static_cast<std::ptrdiff_t>(sample.offset),
              fileData.begin() + static_cast<std::ptrdiff_t>(sample.offset + sample.size),
              0xA5);

    auto const temp = ao::test::TempFile{fileData, ".m4a"};
    auto decoderPtr = ao::test::requireValue(AacDecoderSession::open(temp.path, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;
    auto const info = decoder.streamInfo();

    auto const firstBlockRes = decoder.readNextBlock();
    REQUIRE(firstBlockRes);
    CHECK(firstBlockRes->frames > 0);
    CHECK(firstBlockRes->firstFrameIndex == 0);

    auto const secondBlockRes = decoder.readNextBlock();
    REQUIRE_FALSE(secondBlockRes);
    CHECK(secondBlockRes.error().code == Error::Code::DecodeFailed);
    CHECK(decoder.streamInfo() == info);
  }
} // namespace ao::audio::test
