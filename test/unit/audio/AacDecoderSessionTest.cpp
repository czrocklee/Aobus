// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/AacDecoderSession.h>

#include "DecoderTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include <ao/AudioCodec.h>
#include <ao/audio/SampleEncoding.h>

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

    auto decoder = AacDecoderSession{SampleEncoding::Signed16Le};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    CHECK(info.codec == AudioCodec::Aac);
    CHECK(info.duration >= std::chrono::milliseconds{950});
    CHECK(info.sourceFormat.precisionBits == 16);
    CHECK(info.isLossy);

    auto const block = decoder.readNextBlock();
    REQUIRE(block);
    CHECK_FALSE(block->bytes.empty());
    CHECK(block->frames > 0);
    CHECK(block->firstFrameIndex == 0);
  }

  TEST_CASE("AacDecoderSession - seeks within decoded MP4 samples", "[audio][unit][aac][seek]")
  {
    auto const testFile = std::filesystem::path{AUDIO_TEST_DATA_DIR} / "basic_metadata.m4a";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'basic_metadata.m4a' missing");
    }

    auto decoder = AacDecoderSession{SampleEncoding::Signed16Le};
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
    auto const testFile = std::filesystem::path{AUDIO_TEST_DATA_DIR} / "basic_metadata.m4a";

    if (!std::filesystem::exists(testFile))
    {
      SKIP("Test file 'basic_metadata.m4a' missing");
    }

    auto decoder = AacDecoderSession{SampleEncoding::Signed32Le};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    CHECK(info.sourceFormat.precisionBits == 16);
    CHECK(encodingContainerBits(info.outputFormat.encoding) == 32);

    auto const block = decoder.readNextBlock();
    REQUIRE(block);
    CHECK(block->frames > 0);
    CHECK(block->bytes.size() ==
          static_cast<std::size_t>(block->frames) * info.outputFormat.channels * sizeof(std::int32_t));
  }

  TEST_CASE("AacDecoderSession - supports lossless output encodings", "[audio][unit][aac]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.m4a");

    for (auto const encoding : {SampleEncoding::Signed24PackedLe,
                                SampleEncoding::Signed24In32Le,
                                SampleEncoding::Signed32Le,
                                SampleEncoding::Float32Le})
    {
      auto decoder = AacDecoderSession{encoding};
      REQUIRE(decoder.open(testFile));
      CHECK(decoder.streamInfo().outputFormat.encoding == encoding);
    }
  }

  TEST_CASE("AacDecoderSession - reports error paths", "[audio][unit][aac][error]")
  {
    auto const testFile = std::filesystem::path{AUDIO_TEST_DATA_DIR} / "basic_metadata.m4a";

    SECTION("Seek on unopened file")
    {
      auto decoder = AacDecoderSession{SampleEncoding::Signed16Le};
      CHECK(!decoder.seek(std::chrono::milliseconds{100}));
    }

    SECTION("Read on unopened file returns end of stream")
    {
      auto decoder = AacDecoderSession{SampleEncoding::Signed16Le};
      auto const block = decoder.readNextBlock();

      REQUIRE(block);
      CHECK(block->endOfStream);
      CHECK(block->bytes.empty());
    }

    SECTION("Non-existent file")
    {
      auto decoder = AacDecoderSession{SampleEncoding::Signed16Le};
      CHECK(!decoder.open("/path/to/nowhere/nonexistent.m4a"));
    }

    SECTION("Invalid file content")
    {
      auto const tempFile = ao::test::TempFile{".m4a"};
      {
        auto ofs = std::ofstream{tempFile.path, std::ios::binary};
        ofs << "NOT AN AAC FILE! Random garbage data...";
      }

      auto decoder = AacDecoderSession{SampleEncoding::Signed16Le};
      CHECK(!decoder.open(tempFile.path));
    }

    SECTION("Read after close returns end of stream")
    {
      auto decoder = AacDecoderSession{SampleEncoding::Signed16Le};
      REQUIRE(decoder.open(testFile));

      decoder.close();
      decoder.close();
      checkClosedSession(decoder);
    }

    SECTION("Failed reopen clears the previous stream state")
    {
      auto const existingFile = requireAudioFixture("basic_metadata.m4a");
      auto decoder = AacDecoderSession{SampleEncoding::Signed16Le};

      REQUIRE(decoder.open(existingFile));
      CHECK(decoder.streamInfo().sourceFormat.sampleRate > 0);
      CHECK(!decoder.open("/path/to/nowhere/nonexistent.m4a"));
      checkClosedSession(decoder);
    }
  }

  TEST_CASE("AacDecoderSession - reports end of stream", "[audio][unit][aac]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.m4a");

    auto decoder = AacDecoderSession{SampleEncoding::Signed16Le};
    REQUIRE(decoder.open(testFile));

    CHECK(readUntilStableEndOfStream(decoder, 256) > 0);
  }
} // namespace ao::audio::test
