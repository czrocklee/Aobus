// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/detail/MappedFileCursor.h"

#include "test/TestFixtureSupport.h"
#include <ao/Error.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace ao::audio::detail::test
{
  TEST_CASE("MappedFileCursor - reads and seeks mapped bytes")
  {
    auto const data = std::vector<std::uint8_t>{10, 20, 30, 40, 50};
    auto const temp = ao::test::TempFile{data};
    auto cursor = MappedFileCursor{};

    REQUIRE(cursor.open(temp.path));
    CHECK(cursor.isOpen());
    CHECK(cursor.position() == 0);
    CHECK(cursor.size() == data.size());

    auto output = std::array<std::byte, 3>{};
    REQUIRE(cursor.read(output) == output.size());
    CHECK(output == std::array{std::byte{10}, std::byte{20}, std::byte{30}});
    CHECK(cursor.position() == 3);

    REQUIRE(cursor.seek(-2, SeekOrigin::Current));
    CHECK(cursor.position() == 1);
    REQUIRE(cursor.read(output) == output.size());
    CHECK(output == std::array{std::byte{20}, std::byte{30}, std::byte{40}});

    REQUIRE(cursor.seek(-1, SeekOrigin::End));
    CHECK(cursor.position() == 4);
    output.fill(std::byte{0xEE});
    REQUIRE(cursor.read(output) == 1);
    CHECK(output == std::array{std::byte{50}, std::byte{0xEE}, std::byte{0xEE}});
    CHECK(cursor.isAtEnd());

    REQUIRE(cursor.seek(0, SeekOrigin::Begin));
    CHECK(cursor.position() == 0);
    REQUIRE(cursor.read(output) == output.size());
    CHECK(output == std::array{std::byte{10}, std::byte{20}, std::byte{30}});
  }

  TEST_CASE("MappedFileCursor - mapped inspection preserves the native read cursor")
  {
    auto const temp = ao::test::TempFile{std::vector<std::uint8_t>{0x11, 0x22, 0x33, 0x44}};
    auto const next = ao::test::TempFile{std::vector<std::uint8_t>{0xA1, 0xB2}};
    auto cursor = MappedFileCursor{};
    CHECK(cursor.bytes().empty());
    REQUIRE(cursor.open(temp.path));
    auto const borrowed = cursor.bytes();
    auto const expected = std::array{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    CHECK(std::ranges::equal(borrowed, expected));
    CHECK(cursor.position() == 0);
    auto output = std::array<std::byte, 2>{};
    REQUIRE(cursor.read(output) == output.size());
    CHECK(output == std::array{std::byte{0x11}, std::byte{0x22}});
    CHECK(cursor.position() == 2);
    CHECK(cursor.bytes().data() == borrowed.data());
    CHECK(std::ranges::equal(cursor.bytes(), expected));
    CHECK(cursor.position() == 2);
    REQUIRE(cursor.seek(0, SeekOrigin::End));
    CHECK(std::ranges::equal(borrowed, expected));
    CHECK(cursor.position() == expected.size());

    // Reopen ends the old borrow; inspect only the new mapping afterwards.
    REQUIRE(cursor.open(next.path));
    CHECK(cursor.position() == 0);
    CHECK(std::ranges::equal(cursor.bytes(), std::array{std::byte{0xA1}, std::byte{0xB2}}));
    cursor.close();
    CHECK(cursor.bytes().empty());
    CHECK(cursor.position() == 0);
  }

  TEST_CASE("MappedFileCursor - rejects invalid seeks and resets on close")
  {
    auto const data = std::vector<std::uint8_t>{1, 2, 3};
    auto const temp = ao::test::TempFile{data};
    auto cursor = MappedFileCursor{};

    REQUIRE(cursor.open(temp.path));
    REQUIRE(cursor.seek(1, SeekOrigin::Begin));
    auto const requireRejectedSeek = [&](std::int64_t offset, SeekOrigin origin)
    {
      auto const seekRes = cursor.seek(offset, origin);
      REQUIRE_FALSE(seekRes);
      CHECK(seekRes.error().code == Error::Code::SeekFailed);
      CHECK(cursor.position() == 1);
    };
    requireRejectedSeek(-1, SeekOrigin::Begin);
    requireRejectedSeek(1, SeekOrigin::End);
    requireRejectedSeek(std::numeric_limits<std::int64_t>::max(), SeekOrigin::End);

    REQUIRE(cursor.seek(0, SeekOrigin::End));
    CHECK(cursor.isAtEnd());

    auto output = std::array<std::byte, 2>{};
    CHECK(cursor.read(output) == 0);

    cursor.close();
    CHECK_FALSE(cursor.isOpen());
    CHECK(cursor.isAtEnd());
    CHECK(cursor.position() == 0);
    CHECK(cursor.size() == 0);
  }
} // namespace ao::audio::detail::test
