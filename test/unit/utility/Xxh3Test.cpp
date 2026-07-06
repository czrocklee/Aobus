// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/utility/Xxh3.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace ao::utility::test
{
  // Known-answer vectors verified against xxhsum 0.8.3 (`xxhsum -H2` for
  // XXH128, `xxhsum -H3` for XXH3-64); the empty-string XXH3-64 value also
  // matches the official xxHash reference vector.
  TEST_CASE("Xxh3Hash128 - produces stable canonical signatures", "[utility][unit][hash]")
  {
    CHECK(xxh3Hash128Hex("") == "99aa06d3014798d86001c324468d497f");
    CHECK(xxh3Hash128Hex("a") == "a96faf705af16834e6c632b61e964e1f");
    CHECK(xxh3Hash128Hex("hello") == "b5e9c1ad071b3e7fc779cfaa5e523818");
    CHECK(xxh3Hash128Hex("Hello Aobus!") == "a2915abc767ab3c64638778d610cb5db");

    // value() bytes are the XXH128 canonical (big-endian) serialization.
    auto const hash = xxh3Hash128("hello");
    CHECK(hash.bytes[0] == std::byte{0xB5});
    CHECK(hash.bytes[15] == std::byte{0x18});
  }

  TEST_CASE("Xxh3Hash64 - produces stable content keys", "[utility][unit][hash]")
  {
    auto const helloBytes =
      std::array{std::byte{0x68}, std::byte{0x65}, std::byte{0x6C}, std::byte{0x6C}, std::byte{0x6F}};

    CHECK(xxh3Hash64(std::span<std::byte const>{}) == 0x2D06800538D394C2ULL);
    CHECK(xxh3Hash64(helloBytes) == 0x9555E8555C62DCFDULL);
    CHECK(xxh3Hash64("hello") == 0x9555E8555C62DCFDULL);
    CHECK(xxh3Hash64Hex("hello") == "9555e8555c62dcfd");
  }

  TEST_CASE("Xxh3Accumulator64 - matches one-shot hashing across chunks", "[utility][unit][hash]")
  {
    auto accumulator = Xxh3Accumulator64{};

    accumulator.mix("Hello ");
    accumulator.mix("Aobus!");

    CHECK(accumulator.value() == xxh3Hash64("Hello Aobus!"));
    CHECK(accumulator.hex() == xxh3Hash64Hex("Hello Aobus!"));

    // Repeated value() reads are non-destructive.
    CHECK(accumulator.value() == xxh3Hash64("Hello Aobus!"));
  }

  TEST_CASE("Xxh3Hash128 - byte spans match string hashing", "[utility][unit][hash]")
  {
    auto const helloBytes =
      std::array{std::byte{0x68}, std::byte{0x65}, std::byte{0x6C}, std::byte{0x6C}, std::byte{0x6F}};
    auto const highBitByte = std::array{std::byte{0xFF}};
    auto const highBitText = std::string(1, static_cast<char>(0xFF));

    CHECK(xxh3Hash128(std::span<std::byte const>{helloBytes}) == xxh3Hash128("hello"));
    CHECK(xxh3Hash128(std::span<std::byte const>{highBitByte}) == xxh3Hash128(highBitText));
  }

  TEST_CASE("Xxh3Accumulator128 - matches one-shot hashing across chunks", "[utility][unit][hash]")
  {
    auto accumulator = Xxh3Accumulator128{};

    accumulator.mix("Hello ");
    accumulator.mix("Aobus!");

    CHECK(accumulator.value() == xxh3Hash128("Hello Aobus!"));
    CHECK(accumulator.hex() == "a2915abc767ab3c64638778d610cb5db");
  }

  TEST_CASE("Xxh3Accumulator128 - is invariant across chunk boundaries", "[utility][unit][hash]")
  {
    // Larger than the XXH3 internal streaming buffer, so splits exercise the
    // buffered, block-aligned, and long-input code paths.
    auto bytes = std::vector<std::byte>{};
    bytes.reserve(4099);

    for (std::size_t index = 0; index < 4099; ++index)
    {
      bytes.push_back(static_cast<std::byte>((index * 131U + 17U) & 0xFFU));
    }

    auto const all = std::span<std::byte const>{bytes};
    auto const expected = xxh3Hash128(all);

    for (std::size_t const split : {std::size_t{0},
                                    std::size_t{1},
                                    std::size_t{63},
                                    std::size_t{256},
                                    std::size_t{257},
                                    std::size_t{1024},
                                    std::size_t{4098},
                                    bytes.size()})
    {
      auto accumulator = Xxh3Accumulator128{};
      accumulator.mix(all.first(split));
      accumulator.mix(all.subspan(split));
      CHECK(accumulator.value() == expected);
    }

    // Repeated value() reads are non-destructive.
    auto accumulator = Xxh3Accumulator128{};
    accumulator.mix(all);
    CHECK(accumulator.value() == expected);
    CHECK(accumulator.value() == expected);
  }
} // namespace ao::utility::test
