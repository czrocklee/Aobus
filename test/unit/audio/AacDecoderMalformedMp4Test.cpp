// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/media/mp4/TestAtoms.h"
#include <ao/audio/AacDecoderSession.h>
#include <ao/audio/Format.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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
      std::uint32_t sampleSize = 0;
      bool includeAsc = true;
    };

    std::vector<std::uint8_t> makeSyntheticAacMp4(SyntheticAacMp4Options options)
    {
      if (options.sampleSize == 0)
      {
        options.sampleSize = static_cast<std::uint32_t>(options.payload.size());
      }

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
      auto const track = ao::test::mp4::makeCompleteAudioTrackAtom(
        "mp4a", esds, 44100, 44100, options.sampleSize, 1024, options.chunkOffset);
      auto const moov = ao::test::mp4::makeAtom("moov", track);
      data.insert(data.end(), moov.begin(), moov.end());
      return data;
    }
  } // namespace

  TEST_CASE("AacDecoderSession - rejects malformed MP4 sample data", "[audio][unit][aac][error][mp4]")
  {
    SECTION("Sample offset points outside the file")
    {
      auto const data = makeSyntheticAacMp4({
        .payload = {1, 2, 3, 4},
        .chunkOffset = 0x00FF'FFFFU,
      });
      auto const temp = ao::test::TempFile{data, ".m4a"};
      auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      REQUIRE(decoder.open(temp.path));
      CHECK(!decoder.readNextBlock());
    }

    SECTION("Corrupt packet fails direct decode")
    {
      auto const data = makeSyntheticAacMp4({.payload = std::vector<std::uint8_t>(32, 0xA5)});
      auto const temp = ao::test::TempFile{data, ".m4a"};
      auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      REQUIRE(decoder.open(temp.path));
      CHECK(!decoder.readNextBlock());
    }

    SECTION("Corrupt packet fails padded decode")
    {
      auto const data = makeSyntheticAacMp4({.payload = std::vector<std::uint8_t>(32, 0xA5)});
      auto const temp = ao::test::TempFile{data, ".m4a"};
      auto decoder = AacDecoderSession{Format{.bitDepth = 32, .validBits = 16, .isInterleaved = true}};

      REQUIRE(decoder.open(temp.path));
      CHECK(!decoder.readNextBlock());
    }
  }

  TEST_CASE("AacDecoderSession - rejects malformed AudioSpecificConfig", "[audio][unit][aac][error][mp4]")
  {
    SECTION("Missing decoder-specific descriptor")
    {
      auto const data = makeSyntheticAacMp4({.payload = {1, 2, 3, 4}, .includeAsc = false});
      auto const temp = ao::test::TempFile{data, ".m4a"};
      auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      CHECK(!decoder.open(temp.path));
    }

    SECTION("Truncated config")
    {
      auto const data = makeSyntheticAacMp4({.asc = {0x12}, .payload = {1, 2, 3, 4}});
      auto const temp = ao::test::TempFile{data, ".m4a"};
      auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      CHECK(!decoder.open(temp.path));
    }
  }
} // namespace ao::audio::test
