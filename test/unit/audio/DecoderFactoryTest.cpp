// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/media/mp4/TestAtoms.h"
#include <ao/audio/DecoderFactory.h>
#include <ao/audio/Format.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("DecoderFactory - Creates sessions based on extension", "[audio][unit][decoder]")
  {
    auto const format =
      Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false, .isInterleaved = true};

    SECTION("Creates FLAC runtime for .flac")
    {
      auto runtimePtr = createDecoderSession("song.flac", format);
      REQUIRE(runtimePtr != nullptr);
    }

    SECTION("Creates ALAC runtime for MP4 containers with alac sample entries")
    {
      auto const m4a = ao::test::TempFile{ao::test::mp4::makeMinimalAudioMp4("alac"), ".m4a"};
      auto const mp4 = ao::test::TempFile{ao::test::mp4::makeMinimalAudioMp4("alac"), ".mp4"};

      auto session1Ptr = createDecoderSession(m4a.path, format);
      REQUIRE(session1Ptr != nullptr);

      auto session2Ptr = createDecoderSession(mp4.path, format);
      REQUIRE(session2Ptr != nullptr);
    }

    SECTION("Creates ALAC runtime when a video track appears before the audio track")
    {
      auto moovBody = std::vector<std::uint8_t>{};
      auto const videoTrack = ao::test::mp4::makeVideoTrackAtom("avc1");
      auto const audioTrack = ao::test::mp4::makeAudioTrackAtom("alac");
      moovBody.insert(moovBody.end(), videoTrack.begin(), videoTrack.end());
      moovBody.insert(moovBody.end(), audioTrack.begin(), audioTrack.end());

      auto data = std::vector<std::uint8_t>{};
      ao::test::mp4::addAtom(data, "moov", moovBody);
      auto const m4a = ao::test::TempFile{data, ".m4a"};

      REQUIRE(createDecoderSession(m4a.path, format) != nullptr);
    }

    SECTION("Creates AAC runtime for MP4 containers with AAC sample entries")
    {
      auto const m4a = ao::test::TempFile{ao::test::mp4::makeMinimalAudioMp4("mp4a"), ".m4a"};

      REQUIRE(createDecoderSession(m4a.path, format) != nullptr);
    }

    SECTION("Creates MP3 runtime for .mp3")
    {
      auto sessionPtr = createDecoderSession("song.mp3", format);
      REQUIRE(sessionPtr != nullptr);
    }

    SECTION("Returns null for unsupported extensions")
    {
      REQUIRE(createDecoderSession("song.wav", format) == nullptr);
      REQUIRE(createDecoderSession("song.ogg", format) == nullptr);
      REQUIRE(createDecoderSession("missing.m4a", format) == nullptr);
    }

    SECTION("Case-sensitive extension behavior (Current)")
    {
      // The current implementation is case-sensitive on Linux.
      REQUIRE(createDecoderSession("song.FLAC", format) == nullptr);
      REQUIRE(createDecoderSession("song.M4A", format) == nullptr);
    }
  }
} // namespace ao::audio::test
