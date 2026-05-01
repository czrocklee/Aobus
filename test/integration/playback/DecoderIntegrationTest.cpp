// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include <filesystem>
#include <rs/audio/AlacDecoderSession.h>
#include <rs/audio/FlacDecoderSession.h>
#include <rs/utility/Log.h>

using namespace rs::audio;
using namespace rs::audio;

TEST_CASE("FlacDecoderSession handles basic files", "[playback][integration]")
{
  rs::log::Log::init(rs::log::LogLevel::Info);
  auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "basic_metadata.flac";
  if (!std::filesystem::exists(testFile))
  {
    WARN("Test file not found");
    return;
  }

  rs::audio::Format outputFormat;
  outputFormat.bitDepth = 16;
  outputFormat.isInterleaved = true;

  FlacDecoderSession decoder(outputFormat);
  REQUIRE(decoder.open(testFile));

  auto info = decoder.streamInfo();
  CHECK(info.sourceFormat.sampleRate == 44100);
  CHECK(info.sourceFormat.channels == 2);

  auto block = decoder.readNextBlock();
  REQUIRE(block.has_value());
  CHECK(block->frames > 0);
  CHECK_FALSE(block->bytes.empty());

  decoder.close();
}

TEST_CASE("FlacDecoderSession handles S24_32 decoding", "[playback][integration]")
{
  auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "basic_metadata.flac";
  if (!std::filesystem::exists(testFile))
  {
    WARN("Test file not found");
    return;
  }

  // Request 32-bit container with 24-bit precision
  rs::audio::Format outputFormat;
  outputFormat.bitDepth = 32;
  outputFormat.validBits = 24;
  outputFormat.isInterleaved = true;

  FlacDecoderSession decoder(outputFormat);
  REQUIRE(decoder.open(testFile));

  auto info = decoder.streamInfo();
  CHECK(info.outputFormat.bitDepth == 32);
  CHECK(info.outputFormat.validBits == 24);

  auto block = decoder.readNextBlock();
  REQUIRE(block.has_value());
  CHECK(block->bitDepth == 32);
  CHECK(block->bytes.size() == static_cast<std::size_t>(block->frames) * info.sourceFormat.channels * 4U);

  decoder.close();
}

TEST_CASE("AlacDecoderSession handles basic files", "[playback][integration]")
{
  auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "hires.m4a";
  if (!std::filesystem::exists(testFile))
  {
    WARN("Test file not found");
    return;
  }

  rs::audio::Format outputFormat;
  outputFormat.isInterleaved = true;

  AlacDecoderSession decoder(outputFormat);
  REQUIRE(decoder.open(testFile));

  auto const info = decoder.streamInfo();
  CHECK(info.sourceFormat.sampleRate == 96000);
  CHECK(info.sourceFormat.channels == 2);
  CHECK(info.sourceFormat.bitDepth == 24);

  auto block = decoder.readNextBlock();
  REQUIRE(block.has_value());
  CHECK(block->frames > 0);
  CHECK(block->bitDepth == 24);
  CHECK(block->bytes.size() == static_cast<std::size_t>(block->frames) * 2U * 3U);

  decoder.close();
}
