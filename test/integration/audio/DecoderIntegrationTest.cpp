// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/AudioCodec.h>
#include <ao/audio/AlacDecoderSession.h>
#include <ao/audio/DecoderTypes.h>
#include <ao/audio/FlacDecoderSession.h>
#include <ao/audio/Format.h>
#include <ao/audio/IDecoderSession.h>
#include <ao/audio/Mp3DecoderSession.h>
#include <ao/audio/Types.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
      auto const path = std::filesystem::path{TAG_TEST_DATA_DIR} / fileName;

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
    std::vector<T> extractSamples(IDecoderSession& decoder, std::size_t count)
    {
      auto const block = decoder.readNextBlock();

      if (!block || block->bytes.empty())
      {
        return {};
      }

      auto const available = std::min<std::size_t>(count, static_cast<std::size_t>(block->frames) * 2); // Assume Stereo
      auto const* data = reinterpret_cast<T const*>(block->bytes.data());

      return {data, data + available};
    }

    /**
     * @brief Verifies that high bit-depth samples are correctly shifted (bit-perfect)
     * relative to their source bit-depth.
     */
    template<typename TSource, typename TTarget>
    void verifyBitPerfectShift(std::span<TSource const> source, std::span<TTarget const> target, std::uint8_t shift)
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

    void checkPcmBlockLayout(PcmBlock const& block, Format const& outputFormat)
    {
      auto const bytesPerFrame = frameBytes(outputFormat);

      REQUIRE(bytesPerFrame > 0);
      CHECK(block.bitDepth == outputFormat.bitDepth);
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
        auto const format = Format{.bitDepth = 16, .isInterleaved = true};
        auto decoder = FlacDecoderSession{format};
        REQUIRE(decoder.open(testFile));
        CHECK(decoder.streamInfo().codec == AudioCodec::Flac);
        samples16 = extractSamples<std::int16_t>(decoder, 100);
      }

      // 2. Acquire target 32-bit padded samples
      auto samples32 = std::vector<std::int32_t>{};
      {
        auto const format = Format{.bitDepth = 32, .isInterleaved = true};
        auto decoder = FlacDecoderSession{format};
        REQUIRE(decoder.open(testFile));
        samples32 = extractSamples<std::int32_t>(decoder, 100);
      }

      // 3. Verify shift (16 -> 32 should be 16-bit shift)
      verifyBitPerfectShift<std::int16_t, std::int32_t>(samples16, samples32, 16);
    }

    SECTION("ALAC: 24-bit to 32-bit padding alignment")
    {
      auto const testFile = requireAudioFixture("hires.m4a");

      // 1. Acquire reference 24-bit (packed) samples
      auto samples24 = std::vector<std::int32_t>{};
      {
        auto const format = Format{.bitDepth = 24, .isInterleaved = true};
        auto decoder = AlacDecoderSession{format};
        REQUIRE(decoder.open(testFile));
        CHECK(decoder.streamInfo().codec == AudioCodec::Alac);
        auto const block = decoder.readNextBlock();

        REQUIRE(block);
        samples24 = unpackS24(block->bytes);
      }

      // 2. Acquire target 32-bit padded samples
      auto samples32 = std::vector<std::int32_t>{};
      {
        auto const format = Format{.bitDepth = 32, .isInterleaved = true};
        auto decoder = AlacDecoderSession{format};
        REQUIRE(decoder.open(testFile));
        samples32 = extractSamples<std::int32_t>(decoder, samples24.size());
      }

      // 3. Verify shift (24 -> 32 should be 8-bit shift)
      verifyBitPerfectShift<std::int32_t, std::int32_t>(samples24, samples32, 8);
    }
  }

  TEST_CASE("FlacDecoder - fixture decodes with expected integrity", "[audio][integration][flac]")
  {
    auto const testFile = requireAudioFixture("basic_metadata.flac");

    SECTION("Metadata Extraction")
    {
      auto decoder = FlacDecoderSession{Format{.bitDepth = 16}};
      REQUIRE(decoder.open(testFile));

      auto const info = decoder.streamInfo();
      CHECK(info.sourceFormat.sampleRate == 44100);
      CHECK(info.sourceFormat.channels == 2);
      CHECK(info.sourceFormat.bitDepth == 16);
      CHECK(info.outputFormat.sampleRate == 44100);
      CHECK(info.outputFormat.channels == 2);
      CHECK(info.outputFormat.bitDepth == 16);
    }

    SECTION("Seek Consistency")
    {
      auto decoder = FlacDecoderSession{Format{.bitDepth = 16}};
      REQUIRE(decoder.open(testFile));

      auto const info = decoder.streamInfo();
      auto const seekOffset = std::chrono::milliseconds{100};
      auto const expectedFrame = frameIndexAt(info, seekOffset);

      REQUIRE(decoder.seek(seekOffset));
      auto const block = decoder.readNextBlock();

      REQUIRE(block);
      REQUIRE_FALSE(block->endOfStream);
      REQUIRE(block->frames > 0);
      CHECK(block->firstFrameIndex == expectedFrame);
      checkPcmBlockLayout(*block, info.outputFormat);
      checkBlockDoesNotRunPastStream(*block, info);
    }
  }

  TEST_CASE("AlacDecoder - fixture decodes with expected integrity", "[audio][integration][alac]")
  {
    auto const testFile = requireAudioFixture("hires.m4a");

    SECTION("Hires Metadata")
    {
      auto decoder = AlacDecoderSession{Format{}};
      REQUIRE(decoder.open(testFile));

      auto const info = decoder.streamInfo();
      CHECK(info.sourceFormat.sampleRate == 96000);
      CHECK(info.sourceFormat.channels == 2);
      CHECK(info.sourceFormat.bitDepth == 24);
    }
  }

  TEST_CASE("Mp3Decoder - fixture decodes with expected integrity", "[audio][integration][mp3]")
  {
    auto const testFile = requireAudioFixture("hires.mp3");

    SECTION("Metadata Extraction")
    {
      auto decoder = Mp3DecoderSession{Format{.bitDepth = 16}};
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
      auto decoder = FlacDecoderSession{Format{.bitDepth = 16}};
      auto const res = decoder.open(testFile);

      CHECK_FALSE(res);
    }

    SECTION("MP3: Seek near EOF")
    {
      auto const testFile = requireAudioFixture("hires.mp3");
      auto decoder = Mp3DecoderSession{Format{.bitDepth = 16}};
      REQUIRE(decoder.open(testFile));

      auto const info = decoder.streamInfo();

      if (info.duration <= std::chrono::milliseconds{10})
      {
        SKIP("MP3 fixture duration is too short for near-EOF seek");
      }

      auto const seekOffset = info.duration - std::chrono::milliseconds{10};
      auto const expectedFrame = frameIndexAt(info, seekOffset);

      REQUIRE(decoder.seek(seekOffset));
      auto const block = decoder.readNextBlock();
      REQUIRE(block);

      if (block->endOfStream)
      {
        CHECK(block->frames == 0);
        CHECK(block->bytes.empty());
      }
      else
      {
        REQUIRE(block->frames > 0);
        checkNearSeekFrame(*block, expectedFrame, info.sourceFormat.sampleRate);
        checkPcmBlockLayout(*block, info.outputFormat);
        checkBlockDoesNotRunPastStream(*block, info);
      }
    }

    SECTION("Seek near EOF")
    {
      auto const testFile = requireAudioFixture("basic_metadata.flac");
      auto decoder = FlacDecoderSession{Format{.bitDepth = 16}};
      REQUIRE(decoder.open(testFile));

      auto const info = decoder.streamInfo();

      if (info.duration <= std::chrono::milliseconds{10})
      {
        SKIP("FLAC fixture duration is too short for near-EOF seek");
      }

      auto const seekOffset = info.duration - std::chrono::milliseconds{10};
      auto const expectedFrame = frameIndexAt(info, seekOffset);

      REQUIRE(decoder.seek(seekOffset));
      auto const block = decoder.readNextBlock();
      REQUIRE(block);

      if (block->endOfStream && block->frames == 0)
      {
        CHECK(block->bytes.empty());
      }
      else
      {
        REQUIRE(block->frames > 0);
        CHECK(block->firstFrameIndex == expectedFrame);
        checkPcmBlockLayout(*block, info.outputFormat);
        checkBlockDoesNotRunPastStream(*block, info);
      }
    }
  }
} // namespace ao::audio::test
