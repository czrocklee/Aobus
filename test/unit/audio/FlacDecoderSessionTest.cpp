// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/audio/FlacDecoderSession.h"

#include "ao/audio/DecoderTypes.h"
#include "ao/audio/Format.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <ios>

namespace ao::audio::test
{
  TEST_CASE("FlacDecoderSession - Happy Path", "[audio][flac]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "hires.flac";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'hires.flac' missing");
    }

    auto decoder = FlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    REQUIRE(info.sourceFormat.sampleRate > 0);
    REQUIRE(info.durationMs > 0);

    auto const firstBlock = decoder.readNextBlock();
    REQUIRE(firstBlock);
    CHECK(firstBlock->firstFrameIndex == 0);

    REQUIRE(decoder.seek(500));
    auto const soughtBlock = decoder.readNextBlock();
    REQUIRE(soughtBlock);
    CHECK(soughtBlock->firstFrameIndex > 0);

    decoder.flush();
    CHECK(decoder.readNextBlock()); // Should read again from where we were, or next block
  }

  TEST_CASE("FlacDecoderSession - 24-bit", "[audio][flac]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "hires.flac";

    if (!std::filesystem::exists(testFile))
    {
      return;
    }

    auto decoder = FlacDecoderSession{Format{.bitDepth = 24, .isInterleaved = true}};
    REQUIRE(decoder.open(testFile));
    REQUIRE(decoder.readNextBlock());
  }

  TEST_CASE("FlacDecoderSession - Error Paths", "[audio][flac][error]")
  {
    auto decoder = FlacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

    SECTION("Seek on unopened file")
    {
      CHECK(!decoder.seek(100)); // Should fail gracefully
    }

    SECTION("Non-existent file")
    {
      CHECK(!decoder.open("/path/to/nowhere/nonexistent.flac"));
    }

    SECTION("Invalid file content")
    {
      auto const tempFile = std::filesystem::temp_directory_path() / "invalid_flac.flac";
      {
        auto ofs = std::ofstream{tempFile, std::ios::binary};
        ofs << "NOT A FLAC FILE! Random garbage data...";
      }

      CHECK(!decoder.open(tempFile));
      std::filesystem::remove(tempFile);
    }
  }
} // namespace ao::audio::test
