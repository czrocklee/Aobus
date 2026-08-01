// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/DecoderFactory.h>

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/media/mp4/TestAtoms.h"
#include <ao/Error.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("DecoderFactory - creates sessions based on extension", "[audio][unit][decoder]")
  {
    auto const encoding = SampleEncoding::Signed16Le;

    SECTION("Creates FLAC runtime for .flac")
    {
      auto runtime = createDecoderSession("song.flac", encoding);
      REQUIRE(runtime);
      CHECK(*runtime != nullptr);
    }

    SECTION("Creates ALAC runtime for MP4 containers with alac sample entries")
    {
      auto const m4a = ao::test::TempFile{ao::test::mp4::makeMinimalAudioMp4("alac"), ".m4a"};
      auto const mp4 = ao::test::TempFile{ao::test::mp4::makeMinimalAudioMp4("alac"), ".mp4"};

      auto session1 = createDecoderSession(m4a.path, encoding);
      REQUIRE(session1);
      CHECK(*session1 != nullptr);

      auto session2 = createDecoderSession(mp4.path, encoding);
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

      auto session = createDecoderSession(m4a.path, encoding);
      REQUIRE(session);
      CHECK(*session != nullptr);
    }

    SECTION("Creates AAC runtime for MP4 containers with AAC sample entries")
    {
      auto const m4a = ao::test::TempFile{ao::test::mp4::makeMinimalAudioMp4("mp4a"), ".m4a"};

      auto session = createDecoderSession(m4a.path, encoding);
      REQUIRE(session);
      CHECK(*session != nullptr);
    }

    SECTION("Creates MP4 runtime when an extended-size mdat follows the selected track")
    {
      auto data = ao::test::mp4::makeMinimalAudioMp4("alac");
      auto const mdat = ao::test::mp4::makeExtendedAtom("mdat", {1, 2, 3});
      data.insert(data.end(), mdat.begin(), mdat.end());
      auto const m4a = ao::test::TempFile{data, ".m4a"};

      auto session = createDecoderSession(m4a.path, encoding);
      REQUIRE(session);
      CHECK(*session != nullptr);
    }

    SECTION("Creates MP4 runtime when an extended-size mdat precedes the selected track")
    {
      auto data = ao::test::mp4::makeExtendedAtom("mdat", {1, 2, 3});
      auto const movie = ao::test::mp4::makeMinimalAudioMp4("alac");
      data.insert(data.end(), movie.begin(), movie.end());
      auto const m4a = ao::test::TempFile{data, ".m4a"};

      auto session = createDecoderSession(m4a.path, encoding);
      REQUIRE(session);
      CHECK(*session != nullptr);
    }

    SECTION("Creates MP3 runtime for .mp3")
    {
      auto session = createDecoderSession("song.mp3", encoding);
      REQUIRE(session);
      CHECK(*session != nullptr);
    }

    SECTION("Creates WAV runtime for .wav")
    {
      auto session = createDecoderSession("song.wav", encoding);
      REQUIRE(session);
      CHECK(*session != nullptr);
    }

    SECTION("Reports NotSupported for unsupported extensions")
    {
      for (auto const* path : {"song.ogg"})
      {
        auto const result = createDecoderSession(path, encoding);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == Error::Code::NotSupported);
      }
    }

    SECTION("Reports NotSupported for unrecognized MP4 audio codecs")
    {
      auto const m4a = ao::test::TempFile{ao::test::mp4::makeMinimalAudioMp4("ec-3"), ".m4a"};

      auto const result = createDecoderSession(m4a.path, encoding);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    SECTION("Reports NotSupported when an MP4 container has no audio track")
    {
      auto const m4a = ao::test::TempFile{ao::test::mp4::makeAtom("moov", {}), ".m4a"};

      auto const result = createDecoderSession(m4a.path, encoding);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotSupported);
    }

    SECTION("Preserves malformed MP4 structure errors")
    {
      auto const m4a = ao::test::TempFile{std::vector<std::uint8_t>{0, 1, 2}, ".m4a"};

      auto const result = createDecoderSession(m4a.path, encoding);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::CorruptData);
    }

    SECTION("Reports IoError when an MP4 container cannot be read")
    {
      auto const result = createDecoderSession("missing.m4a", encoding);
      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::IoError);
    }

    SECTION("Normalizes decoder extensions before dispatch")
    {
      REQUIRE(createDecoderSession("song.FLAC", encoding));
      REQUIRE(createDecoderSession("song.MP3", encoding));
      REQUIRE(createDecoderSession("song.WAV", encoding));
    }

    SECTION("Normalizes MP4 extensions before probing the container")
    {
      auto const m4a = ao::test::TempFile{ao::test::mp4::makeMinimalAudioMp4("alac"), ".M4A"};
      auto session = createDecoderSession(m4a.path, encoding);

      REQUIRE(session);
      CHECK(*session != nullptr);
    }
  }
} // namespace ao::audio::test
