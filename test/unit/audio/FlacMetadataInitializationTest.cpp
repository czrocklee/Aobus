// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "FlacFrameTestSupport.h"
#include "lib/audio/DecoderFactory.h"
#include "lib/audio/FlacDecoderSession.h"
#include "test/unit/TestFixtureSupport.h"
#include <ao/Error.h>
#include <ao/audio/DecoderSession.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ao::audio::test
{
  namespace
  {
    constexpr auto kMarkers16 = std::to_array<std::int32_t>({16384, -8192});
    constexpr auto kMarkers24 = std::to_array<std::int32_t>({4194305, -2097153});
    // Independently authored interleaved LE PCM; the 24-bit values retain low bits.
    constexpr auto kExpected16 = std::to_array<std::uint8_t>({0x00, 0x40, 0x00, 0xE0});
    constexpr auto kExpected24 = std::to_array<std::uint8_t>({0x01, 0x00, 0x40, 0xFF, 0xFF, 0xDF});

    std::uint8_t computeHeaderCrc8(std::span<std::uint8_t const> bytes)
    {
      std::uint8_t value = 0;

      for (auto const byte : bytes)
      {
        value = static_cast<std::uint8_t>(value ^ byte);

        for (std::uint32_t bit = 0; bit < 8U; ++bit)
        {
          auto const highBitSet = (value & 0x80U) != 0U;
          value = static_cast<std::uint8_t>((value << 1U) ^ (highBitSet ? 0x07U : 0U));
        }
      }

      return value;
    }

    std::uint16_t computeFrameCrc16(std::span<std::uint8_t const> bytes)
    {
      std::uint16_t value = 0;

      for (auto const byte : bytes)
      {
        value = static_cast<std::uint16_t>(value ^ (static_cast<std::uint16_t>(byte) << 8U));

        for (std::uint32_t bit = 0; bit < 8U; ++bit)
        {
          auto const highBitSet = (value & 0x8000U) != 0U;
          value = static_cast<std::uint16_t>((value << 1U) ^ (highBitSet ? 0x8005U : 0U));
        }
      }

      return value;
    }

    void patchImplicitHeader(std::vector<std::uint8_t>& frame, bool implicitRate, bool implicitPrecision)
    {
      // Only the shared writer's frame 0, fixed block of 16, and explicit
      // 48000-Hz rate are supported here: six header bytes precede CRC-8.
      REQUIRE(frame.size() >= 9U);
      REQUIRE(frame[2] == 0x6AU);
      REQUIRE(frame[4] == 0U);
      REQUIRE(frame[5] == 15U);

      if (implicitRate)
      {
        frame[2] &= 0xF0U;
      }

      if (implicitPrecision)
      {
        frame[3] &= 0xF1U;
      }

      frame[6] = computeHeaderCrc8(std::span<std::uint8_t const>{frame}.first(6));
      auto const checksum = computeFrameCrc16(std::span<std::uint8_t const>{frame}.first(frame.size() - 2U));
      frame[frame.size() - 2U] = static_cast<std::uint8_t>(checksum >> 8U);
      frame.back() = static_cast<std::uint8_t>(checksum & 0xFFU);
    }

    std::vector<std::vector<std::uint8_t>> buildNativePrefixes()
    {
      return {
        {},
        {'j', 'u', 'n', 'k'},
        {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 4, 'f', 'L', 'a', 'C'},
        {'j', 'u', 'n', 'k', 'I', 'D', '3', 4, 0, 0, 0, 0, 0, 4, 'f', 'L', 'a', 'C'},
      };
    }

    void checkOpenFailure(std::vector<std::uint8_t> const& bytes, std::string_view diagnostic)
    {
      auto const temp = ao::test::TempFile{bytes, ".flac"};
      auto const concreteRes = FlacDecoderSession::open(temp.path, std::nullopt);
      auto const factoryRes = openDecoderSession(temp.path, std::nullopt);
      REQUIRE_FALSE(concreteRes);
      REQUIRE_FALSE(factoryRes);
      CHECK(concreteRes.error().code == Error::Code::DecodeFailed);
      CHECK(factoryRes.error().code == Error::Code::DecodeFailed);
      CAPTURE(concreteRes.error().message, factoryRes.error().message);
      CHECK(concreteRes.error().message.contains(diagnostic));
      CHECK(factoryRes.error().message.contains(diagnostic));
      CHECK_FALSE(concreteRes.error().message.contains("UNPARSEABLE"));
      CHECK_FALSE(factoryRes.error().message.contains("UNPARSEABLE"));
    }
  } // namespace

  TEST_CASE("FlacDecoderSession - raw implicit header fields require STREAMINFO just like explicit fields",
            "[audio][integration][flac]")
  {
    auto const prefixes = buildNativePrefixes();

    for (std::uint8_t const precisionBits : std::to_array<std::uint8_t>({16, 24}))
    {
      for (bool const implicitRate : {false, true})
      {
        for (bool const implicitPrecision : {false, true})
        {
          auto const markers = precisionBits == 24U ? std::span<std::int32_t const>{kMarkers24}
                                                    : std::span<std::int32_t const>{kMarkers16};
          auto frame = buildFlacVerbatimFrameBytes(0, 48000, 2, precisionBits, markers);
          patchImplicitHeader(frame, implicitRate, implicitPrecision);

          for (std::size_t prefixIndex = 0; prefixIndex < prefixes.size(); ++prefixIndex)
          {
            CAPTURE(precisionBits, implicitRate, implicitPrecision, prefixIndex);
            auto bytes = prefixes[prefixIndex];
            bytes.insert(bytes.end(), frame.begin(), frame.end());
            // Native ID3 handling requires a real container marker after the
            // skipped tag. ID3 followed only by raw frames fails before frame phase.
            checkOpenFailure(
              bytes, prefixIndex < 2U ? "FLAC stream is missing valid STREAMINFO" : "Failed to read FLAC metadata");
          }
        }
      }
    }
  }

  TEST_CASE("FlacDecoderSession - long raw implicit frame input retains the required STREAMINFO cause",
            "[audio][integration][flac]")
  {
    auto const prefixes = buildNativePrefixes();

    for (std::uint8_t const precisionBits : std::to_array<std::uint8_t>({16, 24}))
    {
      auto const markers =
        precisionBits == 24U ? std::span<std::int32_t const>{kMarkers24} : std::span<std::int32_t const>{kMarkers16};
      auto frame = buildFlacVerbatimFrameBytes(0, 48000, 2, precisionBits, markers);
      patchImplicitHeader(frame, true, true);

      for (std::size_t prefixIndex = 0; prefixIndex < prefixes.size(); ++prefixIndex)
      {
        CAPTURE(precisionBits, prefixIndex);
        auto bytes = prefixes[prefixIndex];

        // 4096 copies occupy 307200 or 438272 bytes before the prefix, beyond
        // a native read buffer. Already buffered bytes may still be consumed;
        // public failure alone does not measure read counts or abort latency.
        for (std::size_t repeat = 0; repeat < 4096U; ++repeat)
        {
          bytes.insert(bytes.end(), frame.begin(), frame.end());
        }

        checkOpenFailure(
          bytes, prefixIndex < 2U ? "FLAC stream is missing valid STREAMINFO" : "Failed to read FLAC metadata");
      }
    }
  }

  TEST_CASE("FlacDecoderSession - raw subframe loss of sync retains the missing STREAMINFO cause",
            "[audio][integration][flac]")
  {
    auto frame = buildFlacVerbatimFrameBytes(0, 48000, 2, 16, kMarkers16);
    REQUIRE(frame.size() > 9U);
    frame[7] = 0x80U; // Reserved subframe bit: native READ_FRAME reports LOST_SYNC before write.
    auto const checksum = computeFrameCrc16(std::span<std::uint8_t const>{frame}.first(frame.size() - 2U));
    frame[frame.size() - 2U] = static_cast<std::uint8_t>(checksum >> 8U);
    frame.back() = static_cast<std::uint8_t>(checksum & 0xFFU);
    auto bytes = std::vector<std::uint8_t>{};

    for (std::size_t repeat = 0; repeat < 2048U; ++repeat)
    {
      bytes.insert(bytes.end(), frame.begin(), frame.end());
    }

    checkOpenFailure(bytes, "FLAC stream is missing valid STREAMINFO");
  }

  TEST_CASE("FlacDecoderSession - STREAMINFO resolves implicit frame fields after native prefixes",
            "[audio][integration][flac]")
  {
    auto prefixes = buildNativePrefixes();
    prefixes.emplace_back(32768, std::uint8_t{'j'});

    for (std::uint8_t const precisionBits : std::to_array<std::uint8_t>({16, 24}))
    {
      for (bool const implicitRate : {false, true})
      {
        for (bool const implicitPrecision : {false, true})
        {
          auto const markers = precisionBits == 24U ? std::span<std::int32_t const>{kMarkers24}
                                                    : std::span<std::int32_t const>{kMarkers16};
          auto frame = buildFlacVerbatimFrameBytes(0, 48000, 2, precisionBits, markers);
          patchImplicitHeader(frame, implicitRate, implicitPrecision);
          auto stream = buildFlacStreaminfoBytes(16, 2, precisionBits);
          stream.insert(stream.end(), frame.begin(), frame.end());

          for (std::size_t prefixIndex = 0; prefixIndex < prefixes.size(); ++prefixIndex)
          {
            CAPTURE(precisionBits, implicitRate, implicitPrecision, prefixIndex);
            auto bytes = prefixes[prefixIndex];
            bytes.insert(bytes.end(), stream.begin(), stream.end());
            auto const temp = ao::test::TempFile{bytes, ".flac"};
            auto decoders = std::array<std::unique_ptr<DecoderSession>, 2>{
              ao::test::requireValue(FlacDecoderSession::open(temp.path, std::nullopt)),
              ao::test::requireValue(openDecoderSession(temp.path, std::nullopt)),
            };
            CHECK(decoders[0]->streamInfo() == decoders[1]->streamInfo());

            for (auto const& decoderPtr : decoders)
            {
              auto const info = decoderPtr->streamInfo();
              CHECK(info.sourceFormat.sampleRate == 48000U);
              CHECK(info.sourceFormat.channels == 2U);
              CHECK(info.sourceFormat.precisionBits == precisionBits);
              CHECK(info.optTotalFrames == 16U);
              CHECK(info.outputFormat.encoding ==
                    (precisionBits == 24U ? SampleEncoding::Signed24PackedLe : SampleEncoding::Signed16Le));
              auto const blockRes = decoderPtr->readNextBlock();
              REQUIRE(blockRes);
              CHECK(blockRes->firstFrameIndex == 0U);
              REQUIRE(blockRes->frames == 16U);
              auto const expected = precisionBits == 24U ? std::span<std::uint8_t const>{kExpected24}
                                                         : std::span<std::uint8_t const>{kExpected16};
              REQUIRE(blockRes->bytes.size() == 16U * expected.size());

              for (std::size_t byteIndex = 0; byteIndex < blockRes->bytes.size(); ++byteIndex)
              {
                CAPTURE(byteIndex);
                CHECK(std::to_integer<std::uint8_t>(blockRes->bytes[byteIndex]) ==
                      expected[byteIndex % expected.size()]);
              }

              CHECK(decoderPtr->streamInfo() == info);

              for (std::uint32_t repeat = 0; repeat < 3U; ++repeat)
              {
                auto const endRes = decoderPtr->readNextBlock();
                REQUIRE(endRes);
                CHECK(endRes->endOfStream);
                CHECK(endRes->frames == 0U);
                CHECK(endRes->bytes.empty());
                CHECK(decoderPtr->streamInfo() == info);
              }
            }
          }
        }
      }
    }
  }

  TEST_CASE("FlacDecoderSession - actual metadata truncation is not reclassified as a raw frame failure",
            "[audio][integration][flac]")
  {
    auto bytes = buildFlacStreaminfoBytes(16);

    SECTION("Truncated first STREAMINFO has not published STREAMINFO")
    {
      bytes.pop_back();
    }

    SECTION("Truncated padding without STREAMINFO remains a metadata read failure")
    {
      bytes = {'f', 'L', 'a', 'C', 0x81, 0, 0, 4, 0, 0, 0};
    }

    SECTION("Truncated later padding follows a complete STREAMINFO")
    {
      bytes[4] &= 0x7FU;
      bytes.insert(bytes.end(), {0x81, 0, 0, 4, 0, 0, 0});
    }

    // The wider seven-section native truncation matrix remains in
    // FlacCommentCompatibilityTest.cpp; these rows guard cause precedence.
    checkOpenFailure(bytes, "Failed to read FLAC metadata");
  }
} // namespace ao::audio::test
