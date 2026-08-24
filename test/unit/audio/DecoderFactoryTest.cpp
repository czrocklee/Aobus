// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/DecoderFactory.h"

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/media/mp4/TestAtoms.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("DecoderFactory - opens ready sessions for supported audio files", "[audio][unit][decoder]")
  {
    struct Case final
    {
      std::string_view fileName;
      AudioCodec codec = AudioCodec::Unknown;
    };

    constexpr auto kCases = std::array{
      Case{.fileName = "basic_metadata.flac", .codec = AudioCodec::Flac},
      Case{.fileName = "alac16.m4a", .codec = AudioCodec::Alac},
      Case{.fileName = "basic_metadata.m4a", .codec = AudioCodec::Aac},
      Case{.fileName = "basic_metadata.mp3", .codec = AudioCodec::Mp3},
      Case{.fileName = "basic_metadata.wav", .codec = AudioCodec::Wav},
      Case{.fileName = "basic_metadata.opus", .codec = AudioCodec::Opus},
    };

    for (auto const& testCase : kCases)
    {
      auto const fixture = requireAudioFixture(testCase.fileName);
      auto inspectionRes = openDecoderSession(fixture, std::nullopt);

      REQUIRE(inspectionRes);
      REQUIRE(*inspectionRes != nullptr);
      auto const inspectionInfo = (*inspectionRes)->streamInfo();
      CHECK(inspectionInfo.codec == testCase.codec);
      CHECK(inspectionInfo.sourceFormat.sampleRate > 0);
      CHECK(inspectionInfo.sourceFormat.channels > 0);
      CHECK(inspectionInfo.outputFormat.encoding != SampleEncoding::Unknown);

      auto sessionRes = openDecoderSession(fixture, SampleEncoding::Signed16Le);

      REQUIRE(sessionRes);
      REQUIRE(*sessionRes != nullptr);
      auto const info = (*sessionRes)->streamInfo();
      CHECK(info.codec == testCase.codec);
      CHECK(info.sourceFormat.sampleRate > 0);
      CHECK(info.sourceFormat.channels > 0);
      CHECK(info.outputFormat.encoding == SampleEncoding::Signed16Le);
    }
  }

  TEST_CASE("DecoderFactory - reports selection and initialization failures", "[audio][unit][decoder][error]")
  {
    SECTION("Unsupported extension")
    {
      for (auto const path : std::array<std::string_view, 2>{"song.ogg", "video.mp4"})
      {
        CAPTURE(path);
        auto const result = openDecoderSession(path, SampleEncoding::Signed16Le);

        REQUIRE_FALSE(result);
        CHECK(result.error().code == Error::Code::NotSupported);
      }
    }

    SECTION("Unrecognized MP4 audio codec")
    {
      auto const m4a = ao::test::TempFile{ao::test::mp4::makeMinimalAudioMp4("ec-3"), ".m4a"};
      auto const result = openDecoderSession(m4a.path, SampleEncoding::Signed16Le);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    SECTION("MP4 container without an audio track")
    {
      auto const m4a = ao::test::TempFile{ao::test::mp4::makeAtom("moov", {}), ".m4a"};
      auto const result = openDecoderSession(m4a.path, SampleEncoding::Signed16Le);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    SECTION("Malformed MP4 structure")
    {
      auto const m4a = ao::test::TempFile{std::vector<std::uint8_t>{0, 1, 2}, ".m4a"};
      auto const result = openDecoderSession(m4a.path, SampleEncoding::Signed16Le);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("Missing supported file")
    {
      auto const result = openDecoderSession("missing.flac", SampleEncoding::Signed16Le);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::IoError);
    }

    SECTION("Existing malformed supported file")
    {
      auto const flac = ao::test::TempFile{std::vector<std::uint8_t>{0, 1, 2}, ".flac"};
      auto const result = openDecoderSession(flac.path, SampleEncoding::Signed16Le);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::DecodeFailed);
    }
  }

  TEST_CASE("DecoderFactory - normalizes supported extensions before opening", "[audio][unit][decoder]")
  {
    struct Case final
    {
      std::string_view sourceName;
      std::string_view suffix;
      AudioCodec codec = AudioCodec::Unknown;
    };

    constexpr auto kCases = std::array{
      Case{.sourceName = "basic_metadata.flac", .suffix = ".FLAC", .codec = AudioCodec::Flac},
      Case{.sourceName = "basic_metadata.m4a", .suffix = ".M4A", .codec = AudioCodec::Aac},
      Case{.sourceName = "basic_metadata.mp3", .suffix = ".MP3", .codec = AudioCodec::Mp3},
      Case{.sourceName = "basic_metadata.wav", .suffix = ".WAV", .codec = AudioCodec::Wav},
      Case{.sourceName = "basic_metadata.opus", .suffix = ".OPUS", .codec = AudioCodec::Opus},
    };

    for (auto const& testCase : kCases)
    {
      auto const temp = ao::test::TempFile{readFileBytes(requireAudioFixture(testCase.sourceName)), testCase.suffix};
      auto sessionRes = openDecoderSession(temp.path, SampleEncoding::Signed16Le);

      REQUIRE(sessionRes);
      REQUIRE(*sessionRes != nullptr);
      CHECK((*sessionRes)->streamInfo().codec == testCase.codec);
    }
  }
} // namespace ao::audio::test
