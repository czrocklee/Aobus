// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/audio/AlacDecoderSession.h"
#include "lib/audio/FlacDecoderSession.h"
#include "lib/audio/Mp3DecoderSession.h"
#include <ao/AudioCodec.h>
#include <ao/audio/AudioTime.h>
#include <ao/audio/DecodedStreamInfo.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    /**
     * @brief Requires an integration audio fixture, or marks the section skipped.
     */
    std::filesystem::path requireAudioFixture(char const* fileName)
    {
      auto const path = std::filesystem::path{AUDIO_TEST_DATA_DIR} / fileName;

      if (!std::filesystem::exists(path))
      {
        SKIP("Required audio fixture missing: " << path);
      }

      return path;
    }

    /**
     * @brief Extracts a specific number of samples from a decoder runtime for verification.
     */
    template<typename T>
    std::vector<T> extractSamples(DecoderSession& decoder, std::size_t count)
    {
      auto const blockRes = decoder.readNextBlock();

      if (!blockRes || blockRes->bytes.empty())
      {
        return {};
      }

      auto const available =
        std::min<std::size_t>(count, static_cast<std::size_t>(blockRes->frames) * 2); // Assume Stereo
      auto const* data = reinterpret_cast<T const*>(blockRes->bytes.data());

      return {data, data + available};
    }

    /**
     * @brief Verifies that high bit-depth samples are correctly shifted (bit-perfect)
     * relative to their source bit-depth.
     */
    template<typename TSource, typename TTarget>
    void checkBitPerfectShift(std::span<TSource const> source, std::span<TTarget const> target, std::uint8_t shift)
    {
      REQUIRE(source.size() == target.size());

      for (std::size_t i = 0; i < source.size(); ++i)
      {
        if (source[i] != 0)
        {
          auto const expected = static_cast<TTarget>(source[i]) << shift;
          CHECK(target[i] == expected);
        }
      }
    }

    /**
     * @brief Helper for ALAC 24-bit unpacking from 3-byte packed PCM.
     */
    std::vector<std::int32_t> unpackS24(std::span<std::byte const> bytes)
    {
      auto samples = std::vector<std::int32_t>{};

      for (std::size_t i = 0; i + 2 < bytes.size(); i += 3)
      {
        std::int32_t val = static_cast<std::uint8_t>(bytes[i]) | (static_cast<std::uint8_t>(bytes[i + 1]) << 8) |
                           (static_cast<std::uint8_t>(bytes[i + 2]) << 16);

        // Sign extend from 24 to 32 bits
        if ((val & 0x800000) != 0)
        {
          val |= static_cast<std::int32_t>(0xFF000000);
        }

        samples.push_back(val);
      }

      return samples;
    }

    std::uint64_t frameIndexAt(DecodedStreamInfo const& info, std::chrono::milliseconds offset)
    {
      return durationToSamples(offset, info.sourceFormat.sampleRate);
    }

    std::uint64_t totalFrames(DecodedStreamInfo const& info)
    {
      return durationToSamples(info.duration, info.sourceFormat.sampleRate);
    }

    void checkPcmBlockLayout(PcmBlock const& block, PcmFormat const& outputFormat)
    {
      auto const bytesPerFrame = frameBytes(outputFormat);

      REQUIRE(bytesPerFrame > 0);
      CHECK(block.bytes.size() == static_cast<std::size_t>(block.frames) * bytesPerFrame);
    }

    void checkBlockDoesNotRunPastStream(PcmBlock const& block, DecodedStreamInfo const& info)
    {
      if (auto const frameCount = totalFrames(info); frameCount > 0 && block.firstFrameIndex <= frameCount)
      {
        CHECK(block.frames <= frameCount - block.firstFrameIndex);
      }
    }

    void checkNearSeekFrame(PcmBlock const& block, std::uint64_t expectedFrame, std::uint32_t sampleRate)
    {
      auto const toleranceFrames = static_cast<std::uint64_t>(sampleRate / 20U);

      CHECK(block.firstFrameIndex + toleranceFrames >= expectedFrame);
      CHECK(block.firstFrameIndex <= expectedFrame + toleranceFrames);
    }
  } // namespace

  TEST_CASE("Decoder - bit-perfect conversions preserve PCM output", "[audio][integration][decoder]")
  {
    SECTION("FLAC: 16-bit to 32-bit padding alignment")
    {
      auto const testFile = requireAudioFixture("basic_metadata.flac");

      // 1. Acquire reference 16-bit samples
      auto samples16 = std::vector<std::int16_t>{};
      {
        auto decoder = FlacDecoderSession{SampleEncoding::Signed16Le};
        REQUIRE(decoder.open(testFile));
        CHECK(decoder.streamInfo().codec == AudioCodec::Flac);
        samples16 = extractSamples<std::int16_t>(decoder, 100);
      }

      // 2. Acquire target 32-bit padded samples
      auto samples32 = std::vector<std::int32_t>{};
      {
        auto decoder = FlacDecoderSession{SampleEncoding::Signed32Le};
        REQUIRE(decoder.open(testFile));
        samples32 = extractSamples<std::int32_t>(decoder, 100);
      }

      // 3. Verify shift (16 -> 32 should be 16-bit shift)
      checkBitPerfectShift<std::int16_t, std::int32_t>(samples16, samples32, 16);
    }

    SECTION("ALAC: 24-bit to 32-bit padding alignment")
    {
      auto const testFile = requireAudioFixture("hires.m4a");

      // 1. Acquire reference 24-bit (packed) samples
      auto samples24 = std::vector<std::int32_t>{};
      {
        auto decoder = AlacDecoderSession{SampleEncoding::Signed24PackedLe};
        REQUIRE(decoder.open(testFile));
        CHECK(decoder.streamInfo().codec == AudioCodec::Alac);
        auto const blockRes = decoder.readNextBlock();

        REQUIRE(blockRes);
        samples24 = unpackS24(blockRes->bytes);
      }

      // 2. Acquire target 32-bit padded samples
      auto samples32 = std::vector<std::int32_t>{};
      {
        auto decoder = AlacDecoderSession{SampleEncoding::Signed32Le};
        REQUIRE(decoder.open(testFile));
        samples32 = extractSamples<std::int32_t>(decoder, samples24.size());
      }

      // 3. Verify shift (24 -> 32 should be 8-bit shift)
      checkBitPerfectShift<std::int32_t, std::int32_t>(samples24, samples32, 8);
    }
  }

  TEST_CASE("FlacDecoder - fixture decodes with expected integrity", "[audio][integration][flac]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.flac");

    SECTION("Metadata Extraction")
    {
      auto decoder = FlacDecoderSession{SampleEncoding::Signed16Le};
      REQUIRE(decoder.open(testFile));

      auto const info = decoder.streamInfo();
      CHECK(info.sourceFormat.sampleRate == 44100);
      CHECK(info.sourceFormat.channels == 2);
      CHECK(info.sourceFormat.precisionBits == 16);
      CHECK(info.outputFormat.sampleRate == 44100);
      CHECK(info.outputFormat.channels == 2);
      CHECK(info.outputFormat.encoding == SampleEncoding::Signed16Le);
    }

    SECTION("Seek Consistency")
    {
      auto decoder = FlacDecoderSession{SampleEncoding::Signed16Le};
      REQUIRE(decoder.open(testFile));

      auto const info = decoder.streamInfo();
      auto const seekOffset = std::chrono::milliseconds{100};
      auto const expectedFrame = frameIndexAt(info, seekOffset);

      REQUIRE(decoder.seek(seekOffset));
      auto const blockRes = decoder.readNextBlock();

      REQUIRE(blockRes);
      REQUIRE_FALSE(blockRes->endOfStream);
      REQUIRE(blockRes->frames > 0);
      CHECK(blockRes->firstFrameIndex == expectedFrame);
      checkPcmBlockLayout(*blockRes, info.outputFormat);
      checkBlockDoesNotRunPastStream(*blockRes, info);
    }
  }

  TEST_CASE("AlacDecoder - fixture decodes with expected integrity", "[audio][integration][alac]")
  {
    auto const testFile = requireAudioFixture("hires.m4a");

    SECTION("Hires Metadata")
    {
      auto decoder = AlacDecoderSession{std::nullopt};
      REQUIRE(decoder.open(testFile));

      auto const info = decoder.streamInfo();
      CHECK(info.sourceFormat.sampleRate == 96000);
      CHECK(info.sourceFormat.channels == 2);
      CHECK(info.sourceFormat.precisionBits == 24);
    }
  }

  TEST_CASE("Mp3Decoder - fixture decodes with expected integrity", "[audio][integration][mp3]")
  {
    auto const testFile = requireAudioFixture("hires.mp3");

    SECTION("Metadata Extraction")
    {
      auto decoder = Mp3DecoderSession{SampleEncoding::Signed16Le};
      REQUIRE(decoder.open(testFile));

      auto const info = decoder.streamInfo();
      CHECK(info.sourceFormat.sampleRate == 48000);
      CHECK(info.sourceFormat.channels == 2);
      CHECK(info.isLossy == true);
    }
  }

  TEST_CASE("Decoder - malformed and unsupported inputs fail without crashing", "[audio][integration][decoder]")
  {
    SECTION("Corrupt: Opening a non-FLAC file as FLAC")
    {
      // Use this source file itself as a fake FLAC
      auto const testFile = std::filesystem::path{__FILE__};
      auto decoder = FlacDecoderSession{SampleEncoding::Signed16Le};
      auto const resRes = decoder.open(testFile);

      CHECK_FALSE(resRes);
    }

    SECTION("MP3: Seek near EOF")
    {
      auto const testFile = requireAudioFixture("hires.mp3");
      auto decoder = Mp3DecoderSession{SampleEncoding::Signed16Le};
      REQUIRE(decoder.open(testFile));

      auto const info = decoder.streamInfo();

      if (info.duration <= std::chrono::milliseconds{10})
      {
        SKIP("MP3 fixture duration is too short for near-EOF seek");
      }

      auto const seekOffset = info.duration - std::chrono::milliseconds{10};
      auto const expectedFrame = frameIndexAt(info, seekOffset);

      REQUIRE(decoder.seek(seekOffset));
      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);

      if (blockRes->endOfStream)
      {
        CHECK(blockRes->frames == 0);
        CHECK(blockRes->bytes.empty());
      }
      else
      {
        REQUIRE(blockRes->frames > 0);
        checkNearSeekFrame(*blockRes, expectedFrame, info.sourceFormat.sampleRate);
        checkPcmBlockLayout(*blockRes, info.outputFormat);
        checkBlockDoesNotRunPastStream(*blockRes, info);
      }
    }

    SECTION("Seek near EOF")
    {
      auto const testFile = requireAudioFixture("basic_metadata.flac");
      auto decoder = FlacDecoderSession{SampleEncoding::Signed16Le};
      REQUIRE(decoder.open(testFile));

      auto const info = decoder.streamInfo();

      if (info.duration <= std::chrono::milliseconds{10})
      {
        SKIP("FLAC fixture duration is too short for near-EOF seek");
      }

      auto const seekOffset = info.duration - std::chrono::milliseconds{10};
      auto const expectedFrame = frameIndexAt(info, seekOffset);

      REQUIRE(decoder.seek(seekOffset));
      auto const blockRes = decoder.readNextBlock();
      REQUIRE(blockRes);

      if (blockRes->endOfStream && blockRes->frames == 0)
      {
        CHECK(blockRes->bytes.empty());
      }
      else
      {
        REQUIRE(blockRes->frames > 0);
        CHECK(blockRes->firstFrameIndex == expectedFrame);
        checkPcmBlockLayout(*blockRes, info.outputFormat);
        checkBlockDoesNotRunPastStream(*blockRes, info);
      }
    }
  }
} // namespace ao::audio::test
