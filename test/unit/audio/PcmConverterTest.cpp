// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <ao/audio/PcmConverter.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

using namespace ao::audio::pcm;

TEST_CASE("PcmConverter: pad (Linear)", "[audio][pcm]")
{
  SECTION("16-bit to 32-bit padding")
  {
    auto const source = std::to_array<std::int16_t>({0x1234, -0x5678, 0x0000, 0x7FFF, -0x8000});
    std::array<std::int32_t, 5> destination{};

    Converter::pad<std::int16_t, std::int32_t>(source, destination, 16);

    CHECK(destination[0] == 0x12340000);
    CHECK(destination[1] == -0x56780000);
    CHECK(destination[2] == 0x00000000);
    CHECK(destination[3] == 0x7FFF0000);
    CHECK(destination[4] == -0x80000000);
  }

  SECTION("24-bit to 32-bit padding")
  {
    auto const source = std::to_array<std::int32_t>({0x123456, -0x123456});
    std::array<std::int32_t, 2> destination{};

    Converter::pad<std::int32_t, std::int32_t>(source, destination, 8);

    CHECK(destination[0] == 0x12345600);
    CHECK(destination[1] == -0x12345600);
  }
}

TEST_CASE("PcmConverter: interleaveAndPad", "[audio][pcm]")
{
  SECTION("Stereo 16-bit to 32-bit interleaved")
  {
    auto const left = std::to_array<std::int16_t>({0x1111, 0x2222});
    auto const right = std::to_array<std::int16_t>({0x3333, 0x4444});

    std::array<std::span<std::int16_t const>, 2> channels{left, right};
    std::array<std::int32_t, 4> destination{};

    Converter::interleaveAndPad<std::int16_t, std::int32_t>(channels, destination, 16);

    CHECK(destination[0] == 0x11110000); // L0
    CHECK(destination[1] == 0x33330000); // R0
    CHECK(destination[2] == 0x22220000); // L1
    CHECK(destination[3] == 0x44440000); // R1
  }
}

TEST_CASE("PcmConverter: unpackS24", "[audio][pcm]")
{
  SECTION("Unpack S24_LE to S32_LE (with 8-bit shift)")
  {
    // 0x123456 -> [0x56, 0x34, 0x12]
    // -1 (0xFFFFFF) -> [0xFF, 0xFF, 0xFF]
    auto const source = std::to_array<std::byte>(
      {std::byte{0x56}, std::byte{0x34}, std::byte{0x12}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}});

    std::array<std::int32_t, 2> destination{};

    Converter::unpackS24(source, destination, 8);

    CHECK(destination[0] == 0x12345600);
    CHECK(destination[1] == -1 * (1 << 8)); // 0xFFFFFF00
  }
}
