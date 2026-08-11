// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/Mp3DecoderSession.h"

#include "DecoderTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include <ao/AudioCodec.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("Mp3DecoderSession - decodes happy path", "[audio][unit][mp3]")
  {
    auto const testFile = requireAudioFixture("hires.mp3");

    auto decoder = Mp3DecoderSession{SampleEncoding::Signed16Le};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    CHECK(info.codec == AudioCodec::Mp3);
    CHECK(info.sourceFormat.sampleRate == 48000);
    CHECK(info.sourceFormat.channels == 2);
    CHECK(info.outputFormat.sampleRate == info.sourceFormat.sampleRate);
    CHECK(info.outputFormat.channels == info.sourceFormat.channels);
    CHECK(encodingContainerBits(info.outputFormat.encoding) == 16);
    CHECK(info.duration > std::chrono::milliseconds{0});

    auto const firstBlockRes = decoder.readNextBlock();
    REQUIRE(firstBlockRes);
    CHECK(firstBlockRes->firstFrameIndex == 0);
    CHECK(firstBlockRes->frames > 0);
    CHECK(!firstBlockRes->bytes.empty());

    REQUIRE(decoder.seek(std::chrono::milliseconds{500}));
    auto const soughtBlockRes = decoder.readNextBlock();
    REQUIRE(soughtBlockRes);
    CHECK(soughtBlockRes->firstFrameIndex > 0);

    decoder.flush();
    CHECK(decoder.readNextBlock());
  }

  TEST_CASE("Mp3DecoderSession - empty output format probes native stream", "[audio][unit][mp3]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.mp3");

    auto decoder = Mp3DecoderSession{std::nullopt};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    CHECK(info.sourceFormat.sampleRate == 44100);
    CHECK(info.sourceFormat.channels == 2);
    CHECK(info.sourceFormat.precisionBits == 16);
    CHECK(signalFormat(info.outputFormat) == info.sourceFormat);
    CHECK(info.outputFormat.encoding == SampleEncoding::Signed16Le);
    CHECK(info.isLossy);

    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK(blockRes->frames > 0);
    CHECK(blockRes->bytes.size() == static_cast<std::size_t>(blockRes->frames) * 2U * 2U);
  }

  TEST_CASE("Mp3DecoderSession - handles floating point output", "[audio][unit][mp3]")
  {
    auto const testFile = requireAudioFixture("hires.mp3");

    // Aobus often uses 32-bit float for internal processing
    auto decoder = Mp3DecoderSession{SampleEncoding::Float32Le};
    REQUIRE(decoder.open(testFile));

    auto const info = decoder.streamInfo();
    CHECK(isFloatEncoding(info.outputFormat.encoding));
    CHECK(encodingContainerBits(info.outputFormat.encoding) == 32);

    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK(blockRes->bytes.size() == static_cast<std::size_t>(blockRes->frames) * 2U * 4U);
  }

  TEST_CASE("Mp3DecoderSession - supports reopening", "[audio][unit][mp3]")
  {
    auto const testFile = requireAudioFixture("hires.mp3");

    auto decoder = Mp3DecoderSession{SampleEncoding::Signed16Le};

    REQUIRE(decoder.open(testFile));
    CHECK(decoder.readNextBlock());

    // Open same file again
    REQUIRE(decoder.open(testFile));
    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    CHECK(blockRes->firstFrameIndex == 0); // Should be reset
  }

  TEST_CASE("Mp3DecoderSession - reads until EOF", "[audio][unit][mp3]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.mp3");

    auto decoder = Mp3DecoderSession{SampleEncoding::Signed16Le};
    REQUIRE(decoder.open(testFile));

    CHECK(readUntilStableEndOfStream(decoder, 512) == 44100);
  }

  TEST_CASE("Mp3DecoderSession - scans VBR streams without seek tables before validating seeks",
            "[audio][regression][mp3]")
  {
    auto const testFile = requireAudioFixture("vbr_no_seek_table.mp3");
    auto decoder = Mp3DecoderSession{SampleEncoding::Signed16Le};

    REQUIRE(decoder.open(testFile));
    auto const info = decoder.streamInfo();
    CHECK(info.sourceFormat.sampleRate == 44100);
    CHECK(info.duration == std::chrono::milliseconds{20088});

    REQUIRE(decoder.seek(std::chrono::seconds{10}));
    auto const soughtBlockRes = decoder.readNextBlock();
    REQUIRE(soughtBlockRes);
    CHECK(soughtBlockRes->firstFrameIndex == 441000);
    CHECK(soughtBlockRes->frames > 0);

    REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
    CHECK(readUntilStableEndOfStream(decoder, 1024) == 885888);
    CHECK_FALSE(decoder.seek(info.duration + std::chrono::milliseconds{1}));
  }

  TEST_CASE("Mp3DecoderSession - rejects a midstream format change", "[audio][unit][mp3][error]")
  {
    auto const firstFile = requireAudioFixture("basic_metadata.mp3");
    auto const secondFile = requireAudioFixture("hires.mp3");
    auto data = readFileBytes(firstFile);
    auto const secondData = readFileBytes(secondFile);
    data.insert(data.end(), secondData.begin(), secondData.end());

    auto const temp = ao::test::TempFile{data, ".mp3"};
    auto decoder = Mp3DecoderSession{SampleEncoding::Signed16Le};
    REQUIRE(decoder.open(temp.path));
    auto const initialInfo = decoder.streamInfo();

    bool rejectedFormatChange = false;

    for (std::int32_t count = 0; count < 512 && !rejectedFormatChange; ++count)
    {
      if (auto const blockRes = decoder.readNextBlock(); !blockRes)
      {
        CHECK(blockRes.error().code == Error::Code::NotSupported);
        rejectedFormatChange = true;
      }
      else if (blockRes->endOfStream)
      {
        break;
      }
    }

    CHECK(rejectedFormatChange);
    CHECK(decoder.streamInfo().outputFormat == initialInfo.outputFormat);

    auto const repeatedReadRes = decoder.readNextBlock();
    REQUIRE_FALSE(repeatedReadRes);
    CHECK(repeatedReadRes.error().code == Error::Code::NotSupported);

    REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
    auto const recoveredBlockRes = decoder.readNextBlock();
    REQUIRE(recoveredBlockRes);
    CHECK(recoveredBlockRes->frames > 0);
  }

  TEST_CASE("Mp3DecoderSession - reports error paths", "[audio][unit][mp3][error]")
  {
    auto decoder = Mp3DecoderSession{SampleEncoding::Signed16Le};

    SECTION("Seek on unopened file")
    {
      CHECK(!decoder.seek(std::chrono::milliseconds{100}));
    }

    SECTION("Non-existent file")
    {
      CHECK(!decoder.open("/path/to/nowhere/nonexistent.mp3"));
    }

    SECTION("Invalid file content")
    {
      auto const tempFile = ao::test::TempFile{".mp3"};
      {
        auto ofs = std::ofstream{tempFile.path, std::ios::binary};
        ofs << "NOT AN MP3 FILE! Random garbage data...";
      }

      auto const result = decoder.open(tempFile.path);
      REQUIRE_FALSE(result);
      CHECK(result.error().message.contains(":"));
      CHECK(result.error().message != "Failed to get MP3 format: A generic mpg123 error.");
    }

    SECTION("Seek way beyond duration")
    {
      auto const testFile = requireAudioFixture("basic_metadata.mp3");
      REQUIRE(decoder.open(testFile));
      // Seek to 1 hour (much longer than basic_metadata.mp3)
      CHECK(!decoder.seek(std::chrono::hours{1}));
    }

    SECTION("Supports lossless wider integer output")
    {
      auto const testFile = requireAudioFixture("basic_metadata.mp3");
      auto int32Decoder = Mp3DecoderSession{SampleEncoding::Signed32Le};

      REQUIRE(int32Decoder.open(testFile));
      CHECK(int32Decoder.streamInfo().outputFormat.encoding == SampleEncoding::Signed32Le);
    }

    SECTION("Supports lossless packed and padded output")
    {
      auto const testFile = requireAudioFixture("basic_metadata.mp3");

      CHECK(Mp3DecoderSession{SampleEncoding::Signed24PackedLe}.open(testFile));
      CHECK(Mp3DecoderSession{SampleEncoding::Signed24In32Le}.open(testFile));
    }

    SECTION("Close and failed reopen clear stream state")
    {
      auto const testFile = requireAudioFixture("basic_metadata.mp3");
      auto lifecycleDecoder = Mp3DecoderSession{SampleEncoding::Signed16Le};

      REQUIRE(lifecycleDecoder.open(testFile));
      lifecycleDecoder.close();
      lifecycleDecoder.close();
      checkClosedSession(lifecycleDecoder);

      REQUIRE(lifecycleDecoder.open(testFile));
      CHECK(!lifecycleDecoder.open("/path/to/nowhere/nonexistent.mp3"));
      checkClosedSession(lifecycleDecoder);
    }
  }
} // namespace ao::audio::test
