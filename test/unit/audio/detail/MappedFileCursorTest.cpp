// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/audio/detail/MappedFileCursor.h"

#include "test/unit/TestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace ao::audio::detail::test
{
  TEST_CASE("MappedFileCursor - reads and seeks mapped bytes", "[audio][unit][detail]")
  {
    auto const data = std::vector<std::uint8_t>{10, 20, 30, 40, 50};
    auto const temp = ao::test::TempFile{data};
    auto cursor = MappedFileCursor{};

    REQUIRE(cursor.open(temp.path));
    CHECK(cursor.isOpen());
    CHECK(cursor.position() == 0);
    CHECK(cursor.size() == data.size());

    auto output = std::array<std::byte, 3>{};
    CHECK(cursor.read(output) == output.size());
    CHECK(output[0] == std::byte{10});
    CHECK(output[2] == std::byte{30});
    CHECK(cursor.position() == 3);

    CHECK(cursor.seek(-2, SeekOrigin::Current));
    CHECK(cursor.position() == 1);
    CHECK(cursor.seek(-1, SeekOrigin::End));
    CHECK(cursor.position() == 4);
    CHECK(cursor.seek(0, SeekOrigin::Begin));
    CHECK(cursor.position() == 0);
  }

  TEST_CASE("MappedFileCursor - rejects invalid seeks and resets on close", "[audio][unit][detail]")
  {
    auto const data = std::vector<std::uint8_t>{1, 2, 3};
    auto const temp = ao::test::TempFile{data};
    auto cursor = MappedFileCursor{};

    REQUIRE(cursor.open(temp.path));
    CHECK(!cursor.seek(-1, SeekOrigin::Begin));
    CHECK(!cursor.seek(1, SeekOrigin::End));
    CHECK(!cursor.seek(std::numeric_limits<std::int64_t>::max(), SeekOrigin::End));

    CHECK(cursor.seek(0, SeekOrigin::End));
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
