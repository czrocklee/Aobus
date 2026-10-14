// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/utility/Uuid.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>

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

  TEST_CASE("parseUuid reads the canonical form in either case", "[utility][unit][uuid]")
  {
    auto const optId = parseUuid("123E4567-e89b-12D3-a456-426614174000");
    REQUIRE(optId);
    CHECK(formatUuid(*optId) == "123e4567-e89b-12d3-a456-426614174000");
    CHECK(parseUuid("00000000-0000-0000-0000-000000000000") == std::optional{UuidBytes{}});
  }

  TEST_CASE("parseUuid rejects anything but the canonical form", "[utility][unit][uuid]")
  {
    CHECK_FALSE(parseUuid(""));
    CHECK_FALSE(parseUuid("123e4567e89b12d3a456426614174000"));
    CHECK_FALSE(parseUuid("123e4567-e89b-12d3-a456-42661417400"));
    CHECK_FALSE(parseUuid("123e4567-e89b-12d3-a456-4266141740000"));
    CHECK_FALSE(parseUuid("123e4567+e89b-12d3-a456-426614174000"));
    CHECK_FALSE(parseUuid("123e4567-e89b-12d3-a456-42661417400g"));
    CHECK_FALSE(parseUuid("-23e4567-e89b-12d3-a456-426614174000"));
    CHECK_FALSE(parseUuid("{23e4567-e89b-12d3-a456-426614174000"));
  }
} // namespace ao::utility::test
