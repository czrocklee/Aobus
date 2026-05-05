// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/AlacDecoderSession.h>
#include <ao/audio/FlacDecoderSession.h>
#include <ao/utility/ByteView.h>
#include <ao/utility/Log.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <span>
#include <vector>

namespace ao::audio
{
  namespace
  {
    /**
     * @brief Extracts a specific number of samples from a decoder session for verification.
     */
    template<typename T>
    auto extractSamples(IDecoderSession& decoder, std::size_t count) -> std::vector<T>
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
    auto unpackS24(std::span<std::byte const> bytes) -> std::vector<std::int32_t>
    {
      std::vector<std::int32_t> samples;

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
  } // namespace

  TEST_CASE("Decoder Bit-Perfect Conversions", "[playback][integration][codec]")
  {
    ao::log::Log::init(ao::log::LogLevel::Warn);

    SECTION("FLAC: 16-bit to 32-bit padding alignment")
    {
      auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "basic_metadata.flac";

      if (!std::filesystem::exists(testFile))
      {
        SKIP("Test file 'basic_metadata.flac' missing");
      }

      // 1. Acquire reference 16-bit samples
      auto samples16 = std::vector<std::int16_t>{};
      {
        auto const format = Format{.bitDepth = 16, .isInterleaved = true};
        auto decoder = FlacDecoderSession{format};
        REQUIRE(decoder.open(testFile));
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
      auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "hires.m4a";

      if (!std::filesystem::exists(testFile))
      {
        SKIP("Test file 'hires.m4a' missing");
      }

      // 1. Acquire reference 24-bit (packed) samples
      auto samples24 = std::vector<std::int32_t>{};
      {
        auto const format = Format{.bitDepth = 24, .isInterleaved = true};
        auto decoder = AlacDecoderSession{format};
        REQUIRE(decoder.open(testFile));
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

  TEST_CASE("FLAC Decoder Integrity", "[playback][integration][flac]")
  {
    auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "basic_metadata.flac";

    if (!std::filesystem::exists(testFile))
    {
      return;
    }

    SECTION("Metadata Extraction")
    {
      auto decoder = FlacDecoderSession{Format{.bitDepth = 16}};
      REQUIRE(decoder.open(testFile));

      auto const info = decoder.streamInfo();
      CHECK(info.sourceFormat.sampleRate == 44100);
      CHECK(info.sourceFormat.channels == 2);
      CHECK(info.sourceFormat.bitDepth == 16);
    }

    SECTION("Seek Consistency")
    {
      auto decoder = FlacDecoderSession{Format{.bitDepth = 16}};
      REQUIRE(decoder.open(testFile));

      REQUIRE(decoder.seek(100)); // Seek to 100ms
      auto const block = decoder.readNextBlock();

      REQUIRE(block);
      CHECK(block->frames > 0);
    }
  }

  TEST_CASE("ALAC Decoder Integrity", "[playback][integration][alac]")
  {
    auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "hires.m4a";

    if (!std::filesystem::exists(testFile))
    {
      return;
    }

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

  TEST_CASE("Decoder Robustness", "[playback][integration][robustness]")
  {
    SECTION("Corrupt: Opening a non-FLAC file as FLAC")
    {
      // Use this source file itself as a fake FLAC
      auto const testFile = std::filesystem::path(__FILE__);
      auto decoder = FlacDecoderSession{Format{.bitDepth = 16}};
      auto const res = decoder.open(testFile);

      CHECK_FALSE(res);
    }

    SECTION("Seek near EOF")
    {
      auto const testFile = std::filesystem::path(TAG_TEST_DATA_DIR) / "basic_metadata.flac";

      if (std::filesystem::exists(testFile))
      {
        auto decoder = FlacDecoderSession{Format{.bitDepth = 16}};
        REQUIRE(decoder.open(testFile));
        auto const info = decoder.streamInfo();

        // Seek to last 10ms
        if (info.durationMs > 10)
        {
          REQUIRE(decoder.seek(info.durationMs - 10));
          auto const block = decoder.readNextBlock();

          // Should either get some frames or EOF immediately
          if (block)
          {
            CHECK(block->frames > 0);
          }
        }
      }
    }
  }
} // namespace ao::audio
