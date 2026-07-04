// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/utility/Fnv1a.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>

namespace ao::utility::test
{
  TEST_CASE("Fnv1a128 - produces stable 128-bit signatures", "[utility][unit][hash]")
  {
    constexpr auto kHashA = fnv1a128("a");
    static_assert(kHashA.bytes[0] == std::byte{0xD2});
    static_assert(kHashA.bytes[15] == std::byte{0x64});

    CHECK(fnv1a128Hex("") == "6c62272e07bb014262b821756295c58d");
    CHECK(fnv1a128Hex("a") == "d228cb696f1a8caf78912b704e4a8964");
    CHECK(fnv1a128Hex("hello") == "e3e1efd54283d94f7081314b599d31b3");
    CHECK(fnv1a128Hex("Hello Aobus!") == "a8616724ace872031cd941f895333568");
  }

  TEST_CASE("Fnv1a128 - byte spans match string hashing", "[utility][unit][hash]")
  {
    auto const helloBytes =
      std::array{std::byte{0x68}, std::byte{0x65}, std::byte{0x6C}, std::byte{0x6C}, std::byte{0x6F}};
    auto const highBitByte = std::array{std::byte{0xFF}};
    auto const highBitText = std::string(1, static_cast<char>(0xFF));

    CHECK(fnv1a128(std::span<std::byte const>{helloBytes}) == fnv1a128("hello"));
    CHECK(fnv1a128(std::span<std::byte const>{highBitByte}) == fnv1a128(highBitText));
  }

  TEST_CASE("Fnv1a128Accumulator - matches one-shot hashing across chunks", "[utility][unit][hash]")
  {
    auto accumulator = Fnv1a128Accumulator{};

    accumulator.mix("Hello ");
    accumulator.mix("Aobus!");

    CHECK(accumulator.value() == fnv1a128("Hello Aobus!"));
    CHECK(accumulator.hex() == "a8616724ace872031cd941f895333568");
  }

  TEST_CASE("Fnv1a128Accumulator - is invariant across chunk boundaries", "[utility][unit][hash]")
  {
    auto const bytes = std::array{std::byte{0x00},
                                  std::byte{0x10},
                                  std::byte{0x20},
                                  std::byte{0x30},
                                  std::byte{0x40},
                                  std::byte{0x50},
                                  std::byte{0xFF}};
    auto const all = std::span<std::byte const>{bytes};

    for (std::size_t split = 0; split <= bytes.size(); ++split)
    {
      auto accumulator = Fnv1a128Accumulator{};
      accumulator.mix(all.first(split));
      accumulator.mix(all.subspan(split));
      CHECK(accumulator.value() == fnv1a128(all));
    }
  }
} // namespace ao::utility::test
