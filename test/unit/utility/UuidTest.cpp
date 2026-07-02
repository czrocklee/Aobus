// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/utility/Uuid.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

namespace ao::utility::test
{
  TEST_CASE("formatUuid renders lowercase canonical groups", "[utility][unit][uuid]")
  {
    auto const id = UuidBytes{std::byte{0x12},
                              std::byte{0x3e},
                              std::byte{0x45},
                              std::byte{0x67},
                              std::byte{0xe8},
                              std::byte{0x9b},
                              std::byte{0x12},
                              std::byte{0xd3},
                              std::byte{0xa4},
                              std::byte{0x56},
                              std::byte{0x42},
                              std::byte{0x66},
                              std::byte{0x14},
                              std::byte{0x17},
                              std::byte{0x40},
                              std::byte{0x00}};

    CHECK(formatUuid(id) == "123e4567-e89b-12d3-a456-426614174000");
  }

  TEST_CASE("formatUuid preserves leading zeroes", "[utility][unit][uuid]")
  {
    CHECK(formatUuid(UuidBytes{}) == "00000000-0000-0000-0000-000000000000");
  }
} // namespace ao::utility::test
