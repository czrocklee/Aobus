// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/AacDecoderSession.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/Format.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>

namespace ao::audio::test
{
  TEST_CASE("AacDecoderSession - Happy Path", "[audio][unit][aac]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.m4a";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'basic_metadata.m4a' missing");
    }

    auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    CHECK(info.durationMs >= 950);
    CHECK(info.sourceFormat.bitDepth == 16);
    CHECK(info.sourceFormat.isInterleaved);
    CHECK(info.isLossy);

    auto const block = decoder.readNextBlock();
    REQUIRE(block);
    CHECK_FALSE(block->bytes.empty());
    CHECK(block->bitDepth == 16);
    CHECK(block->frames > 0);
    CHECK(block->firstFrameIndex == 0);
  }

  TEST_CASE("AacDecoderSession - Seek", "[audio][unit][aac][seek]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.m4a";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'basic_metadata.m4a' missing");
    }

    auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    REQUIRE(info.durationMs > 500);

    REQUIRE(decoder.seek(500));
    auto const block = decoder.readNextBlock();

    REQUIRE(block);
    CHECK(block->frames > 0);
    CHECK(block->firstFrameIndex > 0);
  }

  TEST_CASE("AacDecoderSession - 32-bit padded output", "[audio][unit][aac]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.m4a";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'basic_metadata.m4a' missing");
    }

    auto decoder = AacDecoderSession{Format{.bitDepth = 32, .validBits = 16, .isInterleaved = true}};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    CHECK(info.sourceFormat.bitDepth == 16);
    CHECK(info.sourceFormat.validBits == 16);
    CHECK(info.outputFormat.bitDepth == 32);
    CHECK(info.outputFormat.validBits == 16);

    auto const block = decoder.readNextBlock();
    REQUIRE(block);
    CHECK(block->bitDepth == 32);
    CHECK(block->frames > 0);
    CHECK(block->bytes.size() ==
          static_cast<std::size_t>(block->frames) * info.outputFormat.channels * sizeof(std::int32_t));
  }

  TEST_CASE("AacDecoderSession - Error Paths", "[audio][unit][aac][error]")
  {
    SECTION("Rejects unsupported output bit depth")
    {
      auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.m4a";

      if (!std::filesystem::exists(testFile))
      {
        SKIP("Test file 'basic_metadata.m4a' missing");
      }

      auto decoder = AacDecoderSession{Format{.bitDepth = 24, .isInterleaved = true}};
      CHECK(!decoder.open(testFile));
    }

    SECTION("Seek on unopened file")
    {
      auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
      CHECK(!decoder.seek(100));
    }

    SECTION("Non-existent file")
    {
      auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
      CHECK(!decoder.open("/path/to/nowhere/nonexistent.m4a"));
    }

    SECTION("Invalid file content")
    {
      auto const tempFile = std::filesystem::temp_directory_path() / "invalid_aac.m4a";
      {
        auto ofs = std::ofstream{tempFile, std::ios::binary};
        ofs << "NOT AN AAC FILE! Random garbage data...";
      }

      auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
      CHECK(!decoder.open(tempFile));
      std::filesystem::remove(tempFile);
    }
  }
} // namespace ao::audio::test
