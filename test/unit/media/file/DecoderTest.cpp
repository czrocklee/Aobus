// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/media/file/detail/Decoder.h"

#include "lib/media/file/detail/Content.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <type_traits>

namespace ao::media::file::test
{
  static_assert(std::is_move_constructible_v<detail::Content>);
  static_assert(!std::is_copy_constructible_v<detail::Content>);

  using namespace std::chrono_literals;

  // NOLINTBEGIN(misc-include-cleaner) — <chrono> provides std::operator""ms
  TEST_CASE("Decoder - bitrateFromBytes computes average bitrate", "[media][unit][decoder]")
  {
    // 4,320,000 bytes over 180 s == 192 kbps.
    CHECK(bitrateFromBytes(4'320'000, 180'000ms) == 192'000U);
    // 1,411,200 bytes/s (CD) over exactly one second.
    CHECK(bitrateFromBytes(1'411'200, 1'000ms) == 1'411'200U * 8U);
  }

  TEST_CASE("Decoder - bitrateFromBytes guards non-positive duration", "[media][unit][decoder]")
  {
    CHECK(bitrateFromBytes(1'000'000, 0ms) == 0U);
    CHECK(bitrateFromBytes(1'000'000, -5ms) == 0U);
  }
  // NOLINTEND(misc-include-cleaner)
  TEST_CASE("Decoder - parseSlashPair splits primary/secondary", "[media][unit][decoder]")
  {
    auto const both = parseSlashPair("3/12");
    REQUIRE(both.optPrimary);
    REQUIRE(both.optSecondary);
    CHECK(*both.optPrimary == 3U);
    CHECK(*both.optSecondary == 12U);
  }

  TEST_CASE("Decoder - parseSlashPair handles a bare primary", "[media][unit][decoder]")
  {
    auto const single = parseSlashPair("5");
    REQUIRE(single.optPrimary);
    CHECK(*single.optPrimary == 5U);
    CHECK_FALSE(single.optSecondary);
  }

  TEST_CASE("Decoder - parseSlashPair tolerates missing or invalid components", "[media][unit][decoder]")
  {
    // Empty primary before the slash.
    auto const leadingSlash = parseSlashPair("/7");
    CHECK_FALSE(leadingSlash.optPrimary);
    REQUIRE(leadingSlash.optSecondary);
    CHECK(*leadingSlash.optSecondary == 7U);

    // Wholly empty and non-numeric inputs yield no values.
    CHECK_FALSE(parseSlashPair("").optPrimary);
    CHECK_FALSE(parseSlashPair("abc").optPrimary);

    // Out-of-range for uint16 is rejected (not silently truncated).
    CHECK_FALSE(parseSlashPair("70000").optPrimary);
  }
} // namespace ao::media::file::test
