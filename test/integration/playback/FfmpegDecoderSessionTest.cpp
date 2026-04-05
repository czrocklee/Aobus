// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <app/playback/FfmpegDecoderSession.h>
#include <app/playback/PlaybackTypes.h>

#include <catch2/catch.hpp>

#include <filesystem>
#include <optional>

namespace
{
  // Test fixtures directory - to be set up with actual audio files
  constexpr auto kTestFixtureDir = "/tmp/rockstudio_test_fixtures";
}

namespace app::playback
{

  TEST_CASE("FfmpegDecoderSession construction", "[playback][ffmpeg]")
  {
    StreamFormat outputFormat;
    outputFormat.sampleRate = 44100;
    outputFormat.channels = 2;
    outputFormat.bitDepth = 16;
    outputFormat.isInterleaved = true;

    FfmpegDecoderSession session(outputFormat);
    // Construction should not throw
  }

  TEST_CASE("FfmpegDecoderSession streamInfo before open", "[playback][ffmpeg]")
  {
    StreamFormat outputFormat;
    outputFormat.sampleRate = 44100;
    outputFormat.channels = 2;
    outputFormat.bitDepth = 16;

    FfmpegDecoderSession session(outputFormat);
    auto info = session.streamInfo();

    // Before opening, duration should be 0
    CHECK(info.durationMs == 0);
  }

  TEST_CASE("FfmpegDecoderSession default output format", "[playback][ffmpeg]")
  {
    // When output format has zero values, should use source format
    StreamFormat outputFormat; // All zeros

    FfmpegDecoderSession session(outputFormat);
    auto info = session.streamInfo();

    // Source format should be stored even if no file opened
    CHECK(info.sourceFormat.sampleRate == 0);
  }

  TEST_CASE("FfmpegDecoderSession open valid file", "[playback][ffmpeg]")
  {
    StreamFormat outputFormat;
    outputFormat.sampleRate = 44100;
    outputFormat.channels = 2;
    outputFormat.bitDepth = 16;

    FfmpegDecoderSession session(outputFormat);

    // Create a minimal test fixture path
    auto testFile = std::filesystem::path(kTestFixtureDir) / "test.mp3";

    // This test will only pass if test fixtures exist
    if (std::filesystem::exists(testFile))
    {
      REQUIRE_NOTHROW(session.open(testFile));
      auto info = session.streamInfo();
      CHECK(info.durationMs > 0);
      CHECK(info.sourceFormat.sampleRate > 0);
      CHECK(info.sourceFormat.channels > 0);
    }
    else
    {
      // Skip if fixtures not available
      SUCCEED("Test fixtures not available");
    }
  }

  TEST_CASE("FfmpegDecoderSession close resets state", "[playback][ffmpeg]")
  {
    StreamFormat outputFormat;
    outputFormat.sampleRate = 44100;
    outputFormat.channels = 2;
    outputFormat.bitDepth = 16;

    FfmpegDecoderSession session(outputFormat);

    auto testFile = std::filesystem::path(kTestFixtureDir) / "test.mp3";
    if (std::filesystem::exists(testFile))
    {
      session.open(testFile);
      session.close();

      auto info = session.streamInfo();
      CHECK(info.durationMs == 0);
    }
    else
    {
      SUCCEED("Test fixtures not available");
    }
  }

  TEST_CASE("FfmpegDecoderSession readNextBlock returns nullopt when not open", "[playback][ffmpeg]")
  {
    StreamFormat outputFormat;
    outputFormat.sampleRate = 44100;
    outputFormat.channels = 2;
    outputFormat.bitDepth = 16;

    FfmpegDecoderSession session(outputFormat);

    auto block = session.readNextBlock();
    CHECK(!block.has_value());
  }

  TEST_CASE("PcmBlock structure", "[playback][pcmblock]")
  {
    PcmBlock block;
    CHECK(block.frames == 0);
    CHECK(block.firstFrameIndex == 0);
    CHECK(block.endOfStream == false);
    CHECK(block.samples.empty());
  }

  TEST_CASE("DecodedStreamInfo structure", "[playback][decodedstreaminfo]")
  {
    DecodedStreamInfo info;
    CHECK(info.durationMs == 0);
    CHECK(info.sourceFormat.sampleRate == 0);
    CHECK(info.outputFormat.sampleRate == 0);
  }

} // namespace app::playback
