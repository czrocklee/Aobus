// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/tag/wav/File.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/media/wav/TestWav.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/library/TrackBuilder.h>
#include <ao/utility/Xxh3.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ao::tag::wav::test
{
  namespace
  {
    library::TrackBuilder loadTrack(File const& file)
    {
      auto result = file.loadTrack();
      REQUIRE(result);
      return *result;
    }
  } // namespace

  TEST_CASE("WAV File - maps real INFO tags into TrackBuilder", "[tag][unit][wav][file]")
  {
    auto const file = File{audio::test::requireAudioFixture("basic_metadata.wav")};
    auto builder = loadTrack(file);
    auto const metadata = builder.metadata();

    CHECK(metadata.title() == "Test Title");
    CHECK(metadata.artist() == "Test Artist");
    CHECK(metadata.album() == "Test Album");
    CHECK(metadata.genre() == "Rock");
    CHECK(metadata.year() == 2024);
  }

  TEST_CASE("WAV File - reads real PCM audio properties", "[tag][unit][wav][file]")
  {
    auto const file = File{audio::test::requireAudioFixture("hires.wav")};
    auto builder = loadTrack(file);
    auto const prop = builder.property();

    CHECK(prop.codec() == AudioCodec::Wav);
    CHECK(prop.sampleRate() == 96000);
    CHECK(prop.channels() == 2);
    CHECK(prop.bitDepth() == 24);
    CHECK(prop.duration() >= std::chrono::milliseconds{950});
    CHECK(prop.duration() <= std::chrono::milliseconds{1050});
    CHECK(prop.bitrate() >= 4000000);
  }

  TEST_CASE("WAV File - audio payload range points at real data chunk", "[tag][unit][wav][file]")
  {
    auto const fixture = audio::test::requireAudioFixture("basic_metadata.wav");
    auto const bytes = audio::test::readFileBytes(fixture);
    auto const file = File{fixture};
    auto rangeResult = file.audioPayload();

    REQUIRE(rangeResult);
    auto const range = *rangeResult;
    REQUIRE(range.offset > 0);
    REQUIRE(range.offset + range.bytes.size() <= bytes.size());
    CHECK(range.bytes.size() == static_cast<std::size_t>(44100U) * 2U * 2U);
    CHECK(std::to_integer<std::uint8_t>(range.bytes[0]) == bytes[range.offset]);
    CHECK(std::to_integer<std::uint8_t>(range.bytes[1]) == bytes[range.offset + 1U]);
  }

  TEST_CASE("WAV File - audio payload signature ignores INFO metadata changes", "[tag][unit][wav][file]")
  {
    auto firstData = ao::test::wav::makeWav({
      .audioData = {0x00, 0x00, 0x01, 0x00},
      .infoFields = {{{.id = {'I', 'N', 'A', 'M'}, .value = "Before"}}},
    });
    auto secondData = ao::test::wav::makeWav({
      .audioData = {0x00, 0x00, 0x01, 0x00},
      .infoFields = {{{.id = {'I', 'N', 'A', 'M'}, .value = "After"}}},
    });
    auto const firstTemp = ao::test::TempFile{firstData, ".wav"};
    auto const secondTemp = ao::test::TempFile{secondData, ".wav"};
    auto const firstFile = File{firstTemp.path};
    auto const secondFile = File{secondTemp.path};

    auto firstPayload = firstFile.audioPayload();
    auto secondPayload = secondFile.audioPayload();
    REQUIRE(firstPayload);
    REQUIRE(secondPayload);

    CHECK(utility::xxh3Hash128(firstPayload->bytes) == utility::xxh3Hash128(secondPayload->bytes));
  }

  TEST_CASE("WAV File - rejects malformed input", "[tag][unit][wav][file]")
  {
    SECTION("empty audio data")
    {
      auto data = ao::test::wav::makeWav({.audioData = {}});
      auto const temp = ao::test::TempFile{data, ".wav"};
      auto const file = File{temp.path};
      auto result = file.loadTrack();

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("malformed embedded ID3 tag")
    {
      auto malformedId3 =
        std::vector<std::uint8_t>{'I', 'D', '3', 3, 0, 0, 0, 0, 0, 10, 'T', 'I', 'T', '2', 0, 0, 0, 100, 0, 0};
      auto data = ao::test::wav::makeWav({
        .extraChunks = {{{.id = {'i', 'd', '3', ' '}, .payload = malformedId3}}},
      });
      auto const temp = ao::test::TempFile{data, ".wav"};
      auto const file = File{temp.path};
      auto result = file.loadTrack();

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }
  }
} // namespace ao::tag::wav::test
