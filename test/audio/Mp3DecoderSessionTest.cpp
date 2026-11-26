// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/Mp3DecoderSession.h"

#include "DecoderTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/audio/AudioFixtureSupport.h"
#include <ao/AudioCodec.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    constexpr std::size_t kMonoPcmBytesPerFrame = 2; // Signed 16-bit PCM.
    constexpr std::size_t kVbrFixtureFrames = 885888;

    std::vector<std::byte> collectUninterruptedMonoPcm(std::filesystem::path const& path, std::size_t expectedFrames)
    {
      auto referencePtr = ao::test::requireValue(Mp3DecoderSession::open(path, SampleEncoding::Signed16Le));
      auto referencePcm = std::vector<std::byte>{};
      referencePcm.reserve(expectedFrames * kMonoPcmBytesPerFrame);
      bool reachedEnd = false;

      for (std::size_t index = 0; index < 1024 && !reachedEnd; ++index)
      {
        auto const blockRes = referencePtr->readNextBlock();

        if (!blockRes)
        {
          FAIL("Uninterrupted fixture decode failed: " << blockRes.error().message);
        }

        referencePcm.insert(referencePcm.end(), blockRes->bytes.begin(), blockRes->bytes.end());
        reachedEnd = blockRes->endOfStream;
      }

      REQUIRE(reachedEnd);
      REQUIRE(referencePcm.size() == expectedFrames * kMonoPcmBytesPerFrame);
      return referencePcm;
    }

    enum class SeekCheckExtent : std::uint8_t
    {
      FirstTwoBlocks,
      ThroughEndOfStream,
    };

    void checkSeekAgainstMonoPcm(Mp3DecoderSession& decoder,
                                 std::span<std::byte const> referencePcm,
                                 std::chrono::milliseconds offset,
                                 std::uint64_t expectedFrame,
                                 SeekCheckExtent extent = SeekCheckExtent::FirstTwoBlocks)
    {
      // Restarted synthesis can differ from uninterrupted decoding by one signed-16 LSB.
      constexpr std::int32_t kSeekToleranceLsb = 1;
      CAPTURE(offset);
      REQUIRE(decoder.seek(offset));

      bool const checkThroughEnd = extent == SeekCheckExtent::ThroughEndOfStream;
      auto const totalFrames = referencePcm.size() / kMonoPcmBytesPerFrame;
      REQUIRE(expectedFrame < totalFrames);
      std::int32_t peakReference = 0;

      // Slice the uninterrupted decode by the requested frame, not the reported seek position or block boundaries.
      for (std::size_t index = 0; checkThroughEnd ? expectedFrame < totalFrames : index < 2; ++index)
      {
        CAPTURE(expectedFrame);
        auto const blockRes = decoder.readNextBlock();
        REQUIRE(blockRes);
        CHECK(blockRes->firstFrameIndex == expectedFrame);
        REQUIRE(blockRes->frames > 0);
        REQUIRE(expectedFrame + blockRes->frames <= totalFrames);
        auto const expected = referencePcm.subspan(
          static_cast<std::size_t>(expectedFrame) * kMonoPcmBytesPerFrame, blockRes->frames * kMonoPcmBytesPerFrame);
        REQUIRE(blockRes->bytes.size() == expected.size());
        std::int32_t peakError = 0;

        for (std::size_t sample = 0; sample < blockRes->frames; ++sample)
        {
          auto const expectedSample = static_cast<std::int32_t>(readSigned16LePcmSample(expected, sample));
          auto const error = readSigned16LePcmSample(blockRes->bytes, sample) - expectedSample;
          peakError = std::max(peakError, std::abs(error));
          peakReference = std::max(peakReference, std::abs(expectedSample));
        }

        CHECK(peakError <= kSeekToleranceLsb);
        expectedFrame += blockRes->frames;
      }

      CHECK(peakReference > kSeekToleranceLsb);

      if (checkThroughEnd)
      {
        CHECK(readUntilStableEndOfStream(decoder, 1) == 0);
      }
    }
  } // namespace

  TEST_CASE("Mp3DecoderSession - decodes happy path")
  {
    auto const testFile = requireAudioFixture("hires.mp3");

    auto decoderPtr = ao::test::requireValue(Mp3DecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

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
    auto const flushedBlockRes = decoder.readNextBlock();
    REQUIRE(flushedBlockRes);
    CHECK(flushedBlockRes->frames > 0);
  }

  TEST_CASE("Mp3DecoderSession - empty output format probes native stream")
  {
    auto const testFile = requireAudioFixture("basic_metadata.mp3");

    auto decoderPtr = ao::test::requireValue(Mp3DecoderSession::open(testFile, std::nullopt));
    auto& decoder = *decoderPtr;

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

  TEST_CASE("Mp3DecoderSession - handles floating point output")
  {
    auto const testFile = requireAudioFixture("hires.mp3");

    // Aobus often uses 32-bit float for internal processing
    auto decoderPtr = ao::test::requireValue(Mp3DecoderSession::open(testFile, SampleEncoding::Float32Le));
    auto& decoder = *decoderPtr;

    auto const info = decoder.streamInfo();
    CHECK(isFloatEncoding(info.outputFormat.encoding));
    CHECK(encodingContainerBits(info.outputFormat.encoding) == 32);

    auto const blockRes = decoder.readNextBlock();
    REQUIRE(blockRes);
    REQUIRE(blockRes->frames > 0);
    CHECK(blockRes->bytes.size() == static_cast<std::size_t>(blockRes->frames) * 2U * 4U);
  }

  TEST_CASE("Mp3DecoderSession - reads until EOF")
  {
    auto const testFile = requireAudioFixture("basic_metadata.mp3");

    auto decoderPtr = ao::test::requireValue(Mp3DecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;

    CHECK(readUntilStableEndOfStream(decoder, 512) == 44100);
  }

  TEST_CASE("Mp3DecoderSession - seeks to uninterrupted PCM in a VBR stream without a seek table")
  {
    auto const testFile = requireAudioFixture("vbr_no_seek_table.mp3");
    auto decoderPtr = ao::test::requireValue(Mp3DecoderSession::open(testFile, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;
    auto const info = decoder.streamInfo();
    CHECK(info.sourceFormat.sampleRate == 44100);
    CHECK(info.duration == std::chrono::milliseconds{20088});

    auto const referencePcm = collectUninterruptedMonoPcm(testFile, kVbrFixtureFrames);
    checkSeekAgainstMonoPcm(decoder, referencePcm, std::chrono::seconds{10}, 441000);
    // Backward, inside an MPEG frame after active decoding.
    checkSeekAgainstMonoPcm(decoder, referencePcm, std::chrono::milliseconds{5010}, 220941);
    REQUIRE(decoder.seek(std::chrono::milliseconds{0}));
    CHECK(readUntilStableEndOfStream(decoder, 1024) == kVbrFixtureFrames);
    CHECK_FALSE(decoder.seek(info.duration + std::chrono::milliseconds{1}));
  }

  TEST_CASE("Mp3DecoderSession - seeks to uninterrupted Layer III PCM after a Layer II prefix")
  {
    // Unprotected MPEG1 Layer II at 64 kb/s, 44.1 kHz mono, with no allocated subbands (silence).
    constexpr auto kLayerIiFrame = std::array<std::uint8_t, 208>{0xFF, 0xFD, 0x40, 0xC0};
    constexpr std::size_t kLayerIiFrameCount = 100;
    constexpr std::size_t kPcmFramesPerLayerIiFrame = 1152;
    auto const layerIiiBytes = readFileBytes(requireAudioFixture("vbr_no_seek_table.mp3"));
    auto data = std::vector<std::uint8_t>{};
    data.reserve((kLayerIiFrameCount * kLayerIiFrame.size()) + layerIiiBytes.size());

    for (std::size_t index = 0; index < kLayerIiFrameCount; ++index)
    {
      data.insert(data.end(), kLayerIiFrame.begin(), kLayerIiFrame.end());
    }

    data.insert(data.end(), layerIiiBytes.begin(), layerIiiBytes.end());
    auto const temp = ao::test::TempFile{data, ".mp3"};
    auto decoderPtr = ao::test::requireValue(Mp3DecoderSession::open(temp.path, SampleEncoding::Signed16Le));
    auto const info = decoderPtr->streamInfo();
    CHECK(info.sourceFormat.sampleRate == 44100);
    CHECK(info.sourceFormat.channels == 1);

    auto const referencePcm =
      collectUninterruptedMonoPcm(temp.path, (kLayerIiFrameCount * kPcmFramesPerLayerIiFrame) + kVbrFixtureFrames);
    checkSeekAgainstMonoPcm(*decoderPtr, referencePcm, std::chrono::seconds{10}, 441000);
    checkSeekAgainstMonoPcm(
      *decoderPtr, referencePcm, std::chrono::milliseconds{22666}, 999570, SeekCheckExtent::ThroughEndOfStream);
  }

  TEST_CASE("Mp3DecoderSession - rejects a midstream format change")
  {
    auto const firstFile = requireAudioFixture("basic_metadata.mp3");
    auto const secondFile = requireAudioFixture("hires.mp3");
    auto data = readFileBytes(firstFile);
    auto const secondData = readFileBytes(secondFile);
    data.insert(data.end(), secondData.begin(), secondData.end());

    auto const temp = ao::test::TempFile{data, ".mp3"};
    auto decoderPtr = ao::test::requireValue(Mp3DecoderSession::open(temp.path, SampleEncoding::Signed16Le));
    auto& decoder = *decoderPtr;
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

  TEST_CASE("Mp3DecoderSession - rejects unreadable or malformed input")
  {
    SECTION("Non-existent file")
    {
      auto const tempDir = ao::test::TempDir{};
      CHECK_FALSE(Mp3DecoderSession::open(tempDir.path() / "missing.mp3", SampleEncoding::Signed16Le));
    }

    SECTION("Invalid file content")
    {
      auto const tempFile = ao::test::TempFile{".mp3"};
      {
        auto ofs = std::ofstream{tempFile.path, std::ios::binary};
        ofs << "NOT AN MP3 FILE! Random garbage data...";
      }

      auto const res = Mp3DecoderSession::open(tempFile.path, SampleEncoding::Signed16Le);
      REQUIRE_FALSE(res);
      CHECK(res.error().message.contains(":"));
      CHECK(res.error().message != "Failed to get MP3 format: A generic mpg123 error.");
    }
  }

  TEST_CASE("Mp3DecoderSession - rejects a seek beyond duration")
  {
    auto const testFile = requireAudioFixture("basic_metadata.mp3");
    auto decoderPtr = ao::test::requireValue(Mp3DecoderSession::open(testFile, SampleEncoding::Signed16Le));
    CHECK_FALSE(decoderPtr->seek(std::chrono::hours{1}));
  }

  TEST_CASE("Mp3DecoderSession - supports wider integer carriers")
  {
    auto const encoding =
      GENERATE(SampleEncoding::Signed32Le, SampleEncoding::Signed24PackedLe, SampleEncoding::Signed24In32Le);
    auto const testFile = requireAudioFixture("basic_metadata.mp3");
    auto decoderPtr = ao::test::requireValue(Mp3DecoderSession::open(testFile, encoding));
    CHECK(decoderPtr->streamInfo().outputFormat.encoding == encoding);
    CHECK(decoderPtr->streamInfo().outputFormat.channels == 2);

    auto const blockRes = decoderPtr->readNextBlock();
    REQUIRE(blockRes);
    REQUIRE(blockRes->frames > 0);
    CHECK(blockRes->bytes.size() ==
          static_cast<std::size_t>(blockRes->frames) * 2U * encodingContainerBits(encoding) / 8U);
  }
} // namespace ao::audio::test
