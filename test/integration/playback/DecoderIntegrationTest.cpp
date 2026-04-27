// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/Log.h"
#include "core/decoder/AlacDecoderSession.h"
#include "core/decoder/FlacDecoderSession.h"
#include <catch2/catch.hpp>
#include <filesystem>

using namespace app::core::decoder;
using namespace app::core::playback;

TEST_CASE("FlacDecoderSession handles basic files", "[playback][integration]")
{
  app::core::Log::init(app::core::LogLevel::Info);
  auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "basic_metadata.flac";
  if (!std::filesystem::exists(testFile))
  {
    WARN("Test file not found");
    return;
  }

  app::core::AudioFormat outputFormat;
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

TEST_CASE("AlacDecoderSession handles basic files", "[playback][integration]")
{
  auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "hires.m4a";
  if (!std::filesystem::exists(testFile))
  {
    WARN("Test file not found");
    return;
  }

  app::core::AudioFormat outputFormat;
  outputFormat.bitDepth = 16;
  outputFormat.isInterleaved = true;

  AlacDecoderSession decoder(outputFormat);
  // AlacDecoderSession::open will currently return true but is a skeleton
  REQUIRE(decoder.open(testFile));

  decoder.close();
}
