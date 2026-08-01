// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace ao::audio::test
{
  namespace
  {
    constexpr PcmFormat makeFormat(std::uint32_t sampleRate, std::uint8_t channels, SampleEncoding encoding)
    {
      return PcmFormat{.sampleRate = sampleRate, .channels = channels, .encoding = encoding};
    }
  } // namespace

  TEST_CASE("SampleEncoding - container width is explicit", "[audio][unit][format]")
  {
    CHECK(bytesPerSample(SampleEncoding::Signed16Le) == 2U);
    CHECK(bytesPerSample(SampleEncoding::Signed24PackedLe) == 3U);
    CHECK(bytesPerSample(SampleEncoding::Signed24In32Le) == 4U);
    CHECK(bytesPerSample(SampleEncoding::Signed32Le) == 4U);
    CHECK(bytesPerSample(SampleEncoding::Float32Le) == 4U);
  }

  TEST_CASE("PcmFormat - frameBytes accounts for channel count", "[audio][unit][format]")
  {
    CHECK(frameBytes(makeFormat(44100, 2, SampleEncoding::Signed16Le)) == 4U);
    CHECK(frameBytes(makeFormat(44100, 1, SampleEncoding::Signed16Le)) == 2U);
    CHECK(frameBytes(makeFormat(44100, 2, SampleEncoding::Signed24PackedLe)) == 6U);
    CHECK(frameBytes(makeFormat(48000, 6, SampleEncoding::Signed32Le)) == 24U);
  }

  TEST_CASE("PcmFormat - frameBytes/bytesPerSecond return 0 for unconfigured formats", "[audio][unit][format]")
  {
    CHECK(frameBytes(makeFormat(44100, 0, SampleEncoding::Signed16Le)) == 0U);
    CHECK(frameBytes(PcmFormat{.sampleRate = 44100, .channels = 2}) == 0U);

    CHECK(bytesPerSecond(makeFormat(0, 2, SampleEncoding::Signed16Le)) == 0U);
    CHECK(bytesPerSecond(makeFormat(44100, 0, SampleEncoding::Signed16Le)) == 0U);
    CHECK(bytesPerSecond(PcmFormat{.sampleRate = 44100, .channels = 2}) == 0U);
  }

  TEST_CASE("PcmFormat - bytesPerSecond is sampleRate * frameBytes", "[audio][unit][format]")
  {
    CHECK(bytesPerSecond(makeFormat(44100, 2, SampleEncoding::Signed16Le)) == static_cast<std::uint64_t>(44100) * 4U);
    CHECK(bytesPerSecond(makeFormat(48000, 2, SampleEncoding::Signed24PackedLe)) ==
          static_cast<std::uint64_t>(48000) * 6U);
    CHECK(bytesPerSecond(makeFormat(96000, 2, SampleEncoding::Signed32Le)) == static_cast<std::uint64_t>(96000) * 8U);
  }

  // The helpers must be usable in constant expressions (RT-path callers depend on
  // them folding away).
  static_assert(frameBytes(PcmFormat{.sampleRate = 44100, .channels = 2, .encoding = SampleEncoding::Signed16Le}) ==
                4U);
  static_assert(bytesPerSecond(PcmFormat{.sampleRate = 1000, .channels = 1, .encoding = SampleEncoding::Signed16Le}) ==
                2000U);
} // namespace ao::audio::test
