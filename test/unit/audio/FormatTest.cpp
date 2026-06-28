// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/Format.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace ao::audio::test
{
  namespace
  {
    constexpr Format makeFormat(std::uint32_t sampleRate, std::uint8_t channels, std::uint8_t bitDepth)
    {
      return Format{.sampleRate = sampleRate, .channels = channels, .bitDepth = bitDepth};
    }
  } // namespace

  TEST_CASE("Format - bytesPerSample maps bit depth onto container width", "[audio][unit][format]")
  {
    CHECK(bytesPerSample(makeFormat(44100, 2, 8)) == 2U);
    CHECK(bytesPerSample(makeFormat(44100, 2, 16)) == 2U);
    CHECK(bytesPerSample(makeFormat(44100, 2, 24)) == 3U);
    CHECK(bytesPerSample(makeFormat(44100, 2, 32)) == 4U);

    // Odd widths between 16 and 32 fall into the 4-byte container.
    CHECK(bytesPerSample(makeFormat(44100, 2, 20)) == 4U);
    CHECK(bytesPerSample(makeFormat(44100, 2, 17)) == 4U);
  }

  TEST_CASE("Format - frameBytes accounts for channel count", "[audio][unit][format]")
  {
    CHECK(frameBytes(makeFormat(44100, 2, 16)) == 4U);
    CHECK(frameBytes(makeFormat(44100, 1, 16)) == 2U);
    CHECK(frameBytes(makeFormat(44100, 2, 24)) == 6U);
    CHECK(frameBytes(makeFormat(48000, 6, 32)) == 24U);
  }

  TEST_CASE("Format - frameBytes/bytesPerSecond return 0 for unconfigured formats", "[audio][unit][format]")
  {
    CHECK(frameBytes(makeFormat(44100, 0, 16)) == 0U);
    CHECK(frameBytes(makeFormat(44100, 2, 0)) == 0U);

    CHECK(bytesPerSecond(makeFormat(0, 2, 16)) == 0U);
    CHECK(bytesPerSecond(makeFormat(44100, 0, 16)) == 0U);
    CHECK(bytesPerSecond(makeFormat(44100, 2, 0)) == 0U);
  }

  TEST_CASE("Format - bytesPerSecond is sampleRate * frameBytes", "[audio][unit][format]")
  {
    CHECK(bytesPerSecond(makeFormat(44100, 2, 16)) == static_cast<std::uint64_t>(44100) * 4U);
    CHECK(bytesPerSecond(makeFormat(48000, 2, 24)) == static_cast<std::uint64_t>(48000) * 6U);
    CHECK(bytesPerSecond(makeFormat(96000, 2, 32)) == static_cast<std::uint64_t>(96000) * 8U);
  }

  // The helpers must be usable in constant expressions (RT-path callers depend on
  // them folding away).
  static_assert(frameBytes(Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16}) == 4U);
  static_assert(bytesPerSecond(Format{.sampleRate = 1000, .channels = 1, .bitDepth = 16}) == 2000U);
} // namespace ao::audio::test
