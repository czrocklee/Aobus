// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/DecoderFactory.h>
#include <ao/audio/Format.h>
#include <catch2/catch_test_macros.hpp>

namespace ao::audio::test
{
  TEST_CASE("DecoderFactory - Creates sessions based on extension", "[audio][unit][decoder]")
  {
    auto const format = Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .isFloat = false, .isInterleaved = true};

    SECTION("Creates FLAC session for .flac")
    {
      auto session = createDecoderSession("song.flac", format);
      REQUIRE(session != nullptr);
    }

    SECTION("Creates ALAC session for .m4a and .mp4")
    {
      auto session1 = createDecoderSession("song.m4a", format);
      REQUIRE(session1 != nullptr);

      auto session2 = createDecoderSession("song.mp4", format);
      REQUIRE(session2 != nullptr);
    }

    SECTION("Returns null for unsupported extensions")
    {
      REQUIRE(createDecoderSession("song.mp3", format) == nullptr);
      REQUIRE(createDecoderSession("song.wav", format) == nullptr);
      REQUIRE(createDecoderSession("song.ogg", format) == nullptr);
    }

    SECTION("Case-sensitive extension behavior (Current)")
    {
      // The current implementation is case-sensitive on Linux.
      REQUIRE(createDecoderSession("song.FLAC", format) == nullptr);
      REQUIRE(createDecoderSession("song.M4A", format) == nullptr);
    }
  }
} // namespace ao::audio::test
