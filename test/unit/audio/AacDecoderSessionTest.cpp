// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "DecoderTestSupport.h"
#include "test/unit/TestUtils.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include <ao/AudioCodec.h>
#include <ao/audio/AacDecoderSession.h>
#include <ao/audio/Format.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>

namespace ao::audio::test
{
  TEST_CASE("AacDecoderSession - decodes happy path", "[audio][unit][aac]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.m4a");

    auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    CHECK(info.codec == AudioCodec::Aac);
    CHECK(info.duration >= std::chrono::milliseconds{950});
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

  TEST_CASE("AacDecoderSession - seeks within decoded MP4 samples", "[audio][unit][aac][seek]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.m4a";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'basic_metadata.m4a' missing");
    }

    auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    REQUIRE(info.duration > std::chrono::milliseconds{500});

    REQUIRE(decoder.seek(std::chrono::milliseconds{500}));
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

  TEST_CASE("AacDecoderSession - reports error paths", "[audio][unit][aac][error]")
  {
    auto const testFile = std::filesystem::path{TAG_TEST_DATA_DIR} / "basic_metadata.m4a";

    SECTION("Rejects unsupported output bit depth")
    {
      if (!std::filesystem::exists(testFile))
      {
        SKIP("Test file 'basic_metadata.m4a' missing");
      }

      auto decoder = AacDecoderSession{Format{.bitDepth = 24, .isInterleaved = true}};
      CHECK(!decoder.open(testFile));
    }

    SECTION("Rejects float output")
    {
      if (!std::filesystem::exists(testFile))
      {
        SKIP("Test file 'basic_metadata.m4a' missing");
      }

      auto decoder = AacDecoderSession{Format{.bitDepth = 32, .isFloat = true, .isInterleaved = true}};
      CHECK(!decoder.open(testFile));
    }

    SECTION("Rejects unsupported valid bits")
    {
      if (!std::filesystem::exists(testFile))
      {
        SKIP("Test file 'basic_metadata.m4a' missing");
      }

      auto decoder = AacDecoderSession{Format{.bitDepth = 32, .validBits = 24, .isInterleaved = true}};
      CHECK(!decoder.open(testFile));
    }

    SECTION("Rejects planar output")
    {
      if (!std::filesystem::exists(testFile))
      {
        SKIP("Test file 'basic_metadata.m4a' missing");
      }

      auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = false}};
      CHECK(!decoder.open(testFile));
    }

    SECTION("Rejects sample-rate conversion")
    {
      if (!std::filesystem::exists(testFile))
      {
        SKIP("Test file 'basic_metadata.m4a' missing");
      }

      auto decoder = AacDecoderSession{Format{.sampleRate = 48000, .bitDepth = 16, .isInterleaved = true}};
      CHECK(!decoder.open(testFile));
    }

    SECTION("Rejects channel remapping")
    {
      if (!std::filesystem::exists(testFile))
      {
        SKIP("Test file 'basic_metadata.m4a' missing");
      }

      auto decoder = AacDecoderSession{Format{.channels = 1, .bitDepth = 16, .isInterleaved = true}};
      CHECK(!decoder.open(testFile));
    }

    SECTION("Seek on unopened file")
    {
      auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
      CHECK(!decoder.seek(std::chrono::milliseconds{100}));
    }

    SECTION("Read on unopened file returns end of stream")
    {
      auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
      auto const block = decoder.readNextBlock();

      REQUIRE(block);
      CHECK(block->endOfStream);
      CHECK(block->bytes.empty());
    }

    SECTION("Non-existent file")
    {
      auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
      CHECK(!decoder.open("/path/to/nowhere/nonexistent.m4a"));
    }

    SECTION("Invalid file content")
    {
      auto const tempFile = ao::test::TempFile{".m4a"};
      {
        auto ofs = std::ofstream{tempFile.path, std::ios::binary};
        ofs << "NOT AN AAC FILE! Random garbage data...";
      }

      auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
      CHECK(!decoder.open(tempFile.path));
    }

    SECTION("Read after close returns end of stream")
    {
      auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
      REQUIRE(decoder.open(testFile));

      decoder.close();
      decoder.close();
      checkClosedSession(decoder);
    }

    SECTION("Failed reopen clears the previous stream state")
    {
      auto const existingFile = requireAudioFixture("basic_metadata.m4a");
      auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};

      REQUIRE(decoder.open(existingFile));
      CHECK(decoder.streamInfo().sourceFormat.sampleRate > 0);
      CHECK(!decoder.open("/path/to/nowhere/nonexistent.m4a"));
      checkClosedSession(decoder);
    }
  }

  TEST_CASE("AacDecoderSession - reports end of stream", "[audio][unit][aac]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.m4a");

    auto decoder = AacDecoderSession{Format{.bitDepth = 16, .isInterleaved = true}};
    REQUIRE(decoder.open(testFile));

    CHECK(readUntilStableEndOfStream(decoder, 256) > 0);
  }
} // namespace ao::audio::test
