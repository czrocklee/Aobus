// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/media/mp4/TestAtoms.h"
#include <ao/Error.h>
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
      auto runtime = createDecoderSession("song.flac", format);
      REQUIRE(runtime);
      CHECK(*runtime != nullptr);
    }

    SECTION("Creates ALAC runtime for MP4 containers with alac sample entries")
    {
      auto const m4a = ao::test::TempFile{ao::test::mp4::makeMinimalAudioMp4("alac"), ".m4a"};
      auto const mp4 = ao::test::TempFile{ao::test::mp4::makeMinimalAudioMp4("alac"), ".mp4"};

      auto session1 = createDecoderSession(m4a.path, format);
      REQUIRE(session1);
      CHECK(*session1 != nullptr);

      auto session2 = createDecoderSession(mp4.path, format);
      REQUIRE(session2);
      CHECK(*session2 != nullptr);
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

      auto session = createDecoderSession(m4a.path, format);
      REQUIRE(session);
      CHECK(*session != nullptr);
    }

    SECTION("Creates AAC runtime for MP4 containers with AAC sample entries")
    {
      auto const m4a = ao::test::TempFile{ao::test::mp4::makeMinimalAudioMp4("mp4a"), ".m4a"};

      auto session = createDecoderSession(m4a.path, format);
      REQUIRE(session);
      CHECK(*session != nullptr);
    }

    SECTION("Creates MP3 runtime for .mp3")
    {
      auto session = createDecoderSession("song.mp3", format);
      REQUIRE(session);
      CHECK(*session != nullptr);
    }

    SECTION("Reports NotSupported for unsupported extensions")
    {
      for (auto const* path : {"song.wav", "song.ogg"})
      {
        auto const result = createDecoderSession(path, format);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == Error::Code::NotSupported);
      }
    }

    SECTION("Reports NotSupported for unrecognized MP4 audio codecs")
    {
      auto const m4a = ao::test::TempFile{ao::test::mp4::makeMinimalAudioMp4("ec-3"), ".m4a"};

      auto const result = createDecoderSession(m4a.path, format);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    SECTION("Reports IoError when an MP4 container cannot be read")
    {
      auto const result = createDecoderSession("missing.m4a", format);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::IoError);
    }

    SECTION("Case-sensitive extension behavior (Current)")
    {
      // The current implementation is case-sensitive on Linux.
      CHECK_FALSE(createDecoderSession("song.FLAC", format));
      CHECK_FALSE(createDecoderSession("song.M4A", format));
    }
  }
} // namespace ao::audio::test
