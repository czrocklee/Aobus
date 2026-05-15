// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/audio/PcmConverter.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ao::audio::test
{
  TEST_CASE("PcmConverter: pad (Linear)", "[audio][pcm]")
  {
    SECTION("16-bit to 32-bit padding")
    {
      auto const source = std::to_array<std::int16_t>({0x1234, -0x5678, 0x0000, 0x7FFF, -0x8000});
      auto destination = std::array<std::int32_t, 5>{};

      PcmConverter::pad<std::int16_t, std::int32_t>(source, destination, 16);

      CHECK(destination[0] == 0x12340000);
      CHECK(destination[1] == -0x56780000);
      CHECK(destination[2] == 0x00000000);
      CHECK(destination[3] == 0x7FFF0000);
      CHECK(destination[4] == static_cast<std::int32_t>(-0x80000000)); // NOLINT(modernize-use-integer-sign-comparison)
    }

    SECTION("24-bit to 32-bit padding")
    {
      auto const source = std::to_array<std::int32_t>({0x123456, -0x123456});
      auto destination = std::array<std::int32_t, 2>{};

      PcmConverter::pad<std::int32_t, std::int32_t>(source, destination, 8);

      CHECK(destination[0] == 0x12345600);
      CHECK(destination[1] == -0x12345600);
    }

    SECTION("pad copies only the common sample count")
    {
      auto const source = std::to_array<std::int16_t>({0x1, 0x2, 0x3});
      auto destination = std::array<std::int32_t, 2>{0, 0};

      PcmConverter::pad<std::int16_t, std::int32_t>(source, destination, 16);
      CHECK(destination[0] == 0x10000);
      CHECK(destination[1] == 0x20000);

      auto largeDest = std::array<std::int32_t, 4>{0, 0, 0, 0};
      PcmConverter::pad<std::int16_t, std::int32_t>(source, largeDest, 16);
      CHECK(largeDest[0] == 0x10000);
      CHECK(largeDest[1] == 0x20000);
      CHECK(largeDest[2] == 0x30000);
      CHECK(largeDest[3] == 0x00000);
    }
  }

  TEST_CASE("PcmConverter: interleaveAndPad", "[audio][pcm]")
  {
    SECTION("Stereo 16-bit to 32-bit interleaved")
    {
      auto const left = std::to_array<std::int16_t>({0x1111, 0x2222});
      auto const right = std::to_array<std::int16_t>({0x3333, 0x4444});

      auto const channels = std::array<std::span<std::int16_t const>, 2>{left, right};
      auto destination = std::array<std::int32_t, 4>{};

      PcmConverter::interleaveAndPad<std::int16_t, std::int32_t>(channels, destination, 16);

      CHECK(destination[0] == 0x11110000); // L0
      CHECK(destination[1] == 0x33330000); // R0
      CHECK(destination[2] == 0x22220000); // L1
      CHECK(destination[3] == 0x44440000); // R1
    }

    SECTION("interleaveAndPad returns immediately for empty channel list")
    {
      auto destination = std::array<std::int32_t, 4>{1, 2, 3, 4};
      PcmConverter::interleaveAndPad<std::int16_t, std::int32_t>({}, destination, 16);
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

      PcmConverter::interleaveAndPad<std::int16_t, std::int32_t>(channels, destination, 16);
      CHECK(destination[0] == 0x10000);
      CHECK(destination[1] == 0x40000);
      CHECK(destination[2] == 0x20000);
      CHECK(destination[3] == 0x50000);
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

      auto destination = std::array<std::int32_t, 2>{};

      PcmConverter::unpackS24(source, destination, 8);

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
      PcmConverter::unpackS24(source, destination, 0);

      CHECK(destination[0] == static_cast<std::int32_t>(0xFF800000)); // NOLINT(modernize-use-integer-sign-comparison)
      CHECK(destination[1] == 0x7FFFFF);
      CHECK(destination[2] == -1);
    }
  }
} // namespace ao::audio::test
