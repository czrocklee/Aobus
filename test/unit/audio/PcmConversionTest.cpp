// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "lib/audio/PcmConversion.h"

#include <ao/Error.h>
#include <ao/audio/PcmFormat.h>
#include <ao/audio/SampleEncoding.h>
#include <ao/audio/SignalFormat.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace ao::audio::test
{
  TEST_CASE("PcmConversion - pads linear PCM samples", "[audio][unit][pcm]")
  {
    SECTION("16-bit to 32-bit padding")
    {
      auto const source = std::to_array<std::int16_t>({0x1234, -0x5678, 0x0000, 0x7FFF, -0x8000});
      auto destination = std::array<std::int32_t, 5>{};

      padPcmSamples<std::int16_t, std::int32_t>(source, destination, 16);

      CHECK(destination[0] == 0x12340000);
      CHECK(destination[1] == -0x56780000);
      CHECK(destination[2] == 0x00000000);
      CHECK(destination[3] == 0x7FFF0000);
      CHECK(destination[4] == std::numeric_limits<std::int32_t>::min());
    }

    SECTION("24-bit to 32-bit padding")
    {
      auto const source = std::to_array<std::int32_t>({0x123456, -0x123456});
      auto destination = std::array<std::int32_t, 2>{};

      padPcmSamples<std::int32_t, std::int32_t>(source, destination, 8);

      CHECK(destination[0] == 0x12345600);
      CHECK(destination[1] == -0x12345600);
    }

    SECTION("pad copies only the common sample count")
    {
      auto const source = std::to_array<std::int16_t>({0x1, 0x2, 0x3});
      auto destination = std::array<std::int32_t, 2>{0, 0};

      padPcmSamples<std::int16_t, std::int32_t>(source, destination, 16);
      CHECK(destination[0] == 0x10000);
      CHECK(destination[1] == 0x20000);

      auto largeDestination = std::array<std::int32_t, 4>{0, 0, 0, 0};
      padPcmSamples<std::int16_t, std::int32_t>(source, largeDestination, 16);
      CHECK(largeDestination[0] == 0x10000);
      CHECK(largeDestination[1] == 0x20000);
      CHECK(largeDestination[2] == 0x30000);
      CHECK(largeDestination[3] == 0x00000);
    }
  }

  TEST_CASE("PcmConversion - interleaves channels and pads frames", "[audio][unit][pcm]")
  {
    SECTION("Stereo 16-bit to 32-bit interleaved")
    {
      auto const left = std::to_array<std::int16_t>({0x1111, 0x2222});
      auto const right = std::to_array<std::int16_t>({0x3333, 0x4444});

      auto const channels = std::array<std::span<std::int16_t const>, 2>{left, right};
      auto destination = std::array<std::int32_t, 4>{};

      interleaveAndPadPcmSamples<std::int16_t, std::int32_t>(channels, destination, 16);

      CHECK(destination[0] == 0x11110000); // L0
      CHECK(destination[1] == 0x33330000); // R0
      CHECK(destination[2] == 0x22220000); // L1
      CHECK(destination[3] == 0x44440000); // R1
    }

    SECTION("interleaveAndPad returns immediately for empty channel list")
    {
      auto destination = std::array<std::int32_t, 4>{1, 2, 3, 4};
      interleaveAndPadPcmSamples<std::int16_t, std::int32_t>({}, destination, 16);
      CHECK(destination[0] == 1);
      CHECK(destination[1] == 2);
      CHECK(destination[2] == 3);
      CHECK(destination[3] == 4);
    }

    SECTION("interleaveAndPad truncates to destination frame capacity")
    {
      auto const left = std::to_array<std::int16_t>({0x1, 0x2, 0x3});
      auto const right = std::to_array<std::int16_t>({0x4, 0x5, 0x6});
      auto const channels = std::array<std::span<std::int16_t const>, 2>{left, right};
      auto destination = std::array<std::int32_t, 4>{0, 0, 0, 0}; // only 2 frames capacity

      interleaveAndPadPcmSamples<std::int16_t, std::int32_t>(channels, destination, 16);
      CHECK(destination[0] == 0x10000);
      CHECK(destination[1] == 0x40000);
      CHECK(destination[2] == 0x20000);
      CHECK(destination[3] == 0x50000);
    }
  }

  TEST_CASE("PcmConversion - unpacks signed 24-bit samples", "[audio][unit][pcm]")
  {
    SECTION("Unpack S24_LE to S32_LE (with 8-bit shift)")
    {
      // 0x123456 -> [0x56, 0x34, 0x12]
      // -1 (0xFFFFFF) -> [0xFF, 0xFF, 0xFF]
      auto const source = std::to_array<std::byte>(
        {std::byte{0x56}, std::byte{0x34}, std::byte{0x12}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}});

      auto destination = std::array<std::int32_t, 2>{};

      unpackS24PcmSamples(source, destination, 8);

      CHECK(destination[0] == 0x12345600);
      CHECK(destination[1] == -1 * (1 << 8)); // 0xFFFFFF00
    }

    SECTION("unpackS24 sign-extends negative values correctly")
    {
      // 0x800000 -> [0x00, 0x00, 0x80]
      // 0x7FFFFF -> [0xFF, 0xFF, 0x7F]
      // 0xFFFFFF -> [0xFF, 0xFF, 0xFF]
      auto const source = std::to_array<std::byte>({std::byte{0x00},
                                                    std::byte{0x00},
                                                    std::byte{0x80},
                                                    std::byte{0xFF},
                                                    std::byte{0xFF},
                                                    std::byte{0x7F},
                                                    std::byte{0xFF},
                                                    std::byte{0xFF},
                                                    std::byte{0xFF}});

      auto destination = std::array<std::int32_t, 3>{};
      unpackS24PcmSamples(source, destination, 0);

      CHECK(destination[0] == -0x800000);
      CHECK(destination[1] == 0x7FFFFF);
      CHECK(destination[2] == -1);
    }
  }

  TEST_CASE("convertPcmEncoding - S24-in-32 uses the low 24 bits", "[audio][regression][pcm]")
  {
    auto const source = std::to_array<std::byte>(
      {std::byte{0x56}, std::byte{0x34}, std::byte{0x12}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat =
      PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed24PackedLe};

    auto const result =
      convertPcmEncoding(source, sourceFormat, signalFormat(sourceFormat), SampleEncoding::Signed24In32Le, destination);

    REQUIRE(result);
    CHECK(destination == std::vector<std::byte>{std::byte{0x56},
                                                std::byte{0x34},
                                                std::byte{0x12},
                                                std::byte{0x00},
                                                std::byte{0xFF},
                                                std::byte{0xFF},
                                                std::byte{0xFF},
                                                std::byte{0xFF}});
  }

  TEST_CASE("convertPcmEncoding - S16 to S32 uses the full 32-bit scale", "[audio][regression][pcm]")
  {
    auto const source = std::to_array<std::byte>({std::byte{0x34}, std::byte{0x12}, std::byte{0xFF}, std::byte{0xFF}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat = PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed16Le};

    auto const result =
      convertPcmEncoding(source, sourceFormat, signalFormat(sourceFormat), SampleEncoding::Signed32Le, destination);

    REQUIRE(result);
    CHECK(destination == std::vector<std::byte>{std::byte{0x00},
                                                std::byte{0x00},
                                                std::byte{0x34},
                                                std::byte{0x12},
                                                std::byte{0x00},
                                                std::byte{0x00},
                                                std::byte{0xFF},
                                                std::byte{0xFF}});
  }

  TEST_CASE("convertPcmEncoding - S24 to S32 scales to the full 32-bit range", "[audio][unit][pcm]")
  {
    auto const source = std::to_array<std::byte>({std::byte{0x56}, std::byte{0x34}, std::byte{0x12}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat =
      PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed24PackedLe};

    auto const result =
      convertPcmEncoding(source, sourceFormat, signalFormat(sourceFormat), SampleEncoding::Signed32Le, destination);

    REQUIRE(result);
    CHECK(destination == std::vector<std::byte>{std::byte{0x00}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}});
  }

  TEST_CASE("convertPcmEncoding - integer narrowing is rejected", "[audio][regression][pcm]")
  {
    // 0x123456 and 0xFFFFFF (-1) packed little-endian.
    auto const source = std::to_array<std::byte>(
      {std::byte{0x56}, std::byte{0x34}, std::byte{0x12}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat =
      PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed24PackedLe};

    auto const result =
      convertPcmEncoding(source, sourceFormat, signalFormat(sourceFormat), SampleEncoding::Signed16Le, destination);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotSupported);
    CHECK(destination.empty());
  }

  TEST_CASE("convertPcmEncoding - float source is never quantized to integer PCM", "[audio][regression][pcm]")
  {
    auto const source = std::to_array<std::byte>({std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x3F}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat = PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Float32Le};

    auto const result = convertPcmEncoding(
      source, sourceFormat, signalFormat(sourceFormat), SampleEncoding::Signed24PackedLe, destination);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotSupported);
    CHECK(destination.empty());
  }

  TEST_CASE("convertPcmEncoding - integer PCM converts to exact Float32 samples", "[audio][unit][pcm]")
  {
    auto const source = std::to_array<std::byte>(
      {std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F}, std::byte{0x00}, std::byte{0x00}, std::byte{0x80}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat =
      PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed24PackedLe};

    auto const result =
      convertPcmEncoding(source, sourceFormat, signalFormat(sourceFormat), SampleEncoding::Float32Le, destination);

    REQUIRE(result);
    CHECK(destination == std::vector<std::byte>{std::byte{0xFE},
                                                std::byte{0xFF},
                                                std::byte{0x7F},
                                                std::byte{0x3F},
                                                std::byte{0x00},
                                                std::byte{0x00},
                                                std::byte{0x80},
                                                std::byte{0xBF}});
  }

  TEST_CASE("convertPcmEncoding - Float32 cannot convert losslessly to integer PCM", "[audio][unit][pcm]")
  {
    auto const source = std::to_array<std::byte>({std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x3F}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat = PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Float32Le};

    auto const result =
      convertPcmEncoding(source, sourceFormat, signalFormat(sourceFormat), SampleEncoding::Signed32Le, destination);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotSupported);
    CHECK(destination.empty());
  }

  TEST_CASE("convertPcmEncoding - input must contain complete interleaved frames", "[audio][regression][pcm]")
  {
    auto const source = std::to_array<std::byte>(
      {std::byte{0x01}, std::byte{0x00}, std::byte{0x02}, std::byte{0x00}, std::byte{0x03}, std::byte{0x00}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat = PcmFormat{.sampleRate = 48000, .channels = 2, .encoding = SampleEncoding::Signed16Le};

    auto const result =
      convertPcmEncoding(source, sourceFormat, signalFormat(sourceFormat), SampleEncoding::Signed32Le, destination);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidInput);
    CHECK(destination.empty());
  }

  TEST_CASE("convertPcmEncoding - byte layout and logical signal must describe the same stream", "[audio][unit][pcm]")
  {
    auto const source = std::to_array<std::byte>({std::byte{0x01}, std::byte{0x00}});
    auto destination = std::vector<std::byte>{};
    auto const sourceFormat = PcmFormat{.sampleRate = 48000, .channels = 1, .encoding = SampleEncoding::Signed16Le};
    auto const mismatchedSignal = SignalFormat{.sampleRate = 44100, .channels = 1, .precisionBits = 16};

    auto const result =
      convertPcmEncoding(source, sourceFormat, mismatchedSignal, SampleEncoding::Signed32Le, destination);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidInput);
    CHECK(destination.empty());
  }
} // namespace ao::audio::test
