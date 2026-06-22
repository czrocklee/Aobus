// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/tag/detail/Decoder.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::tag::test
{
  TEST_CASE("Decoder - bitrateFromBytes computes average bitrate", "[tag][unit][decoder]")
  {
    // 4,320,000 bytes over 180 s == 192 kbps.
    CHECK(bitrateFromBytes(4'320'000, std::chrono::milliseconds{180'000}) == 192'000U);
    // 1,411,200 bytes/s (CD) over exactly one second.
    CHECK(bitrateFromBytes(1'411'200, std::chrono::milliseconds{1'000}) == 1'411'200U * 8U);
  }

  TEST_CASE("Decoder - bitrateFromBytes guards non-positive duration", "[tag][unit][decoder]")
  {
    CHECK(bitrateFromBytes(1'000'000, std::chrono::milliseconds{0}) == 0U);
    CHECK(bitrateFromBytes(1'000'000, std::chrono::milliseconds{-5}) == 0U);
  }

  TEST_CASE("Decoder - parseSlashPair splits primary/secondary", "[tag][unit][decoder]")
  {
    auto const both = parseSlashPair("3/12");
    REQUIRE(both.optPrimary.has_value());
    REQUIRE(both.optSecondary.has_value());
    CHECK(*both.optPrimary == 3U);
    CHECK(*both.optSecondary == 12U);
  }

  TEST_CASE("Decoder - parseSlashPair handles a bare primary", "[tag][unit][decoder]")
  {
    auto const single = parseSlashPair("5");
    REQUIRE(single.optPrimary.has_value());
    CHECK(*single.optPrimary == 5U);
    CHECK_FALSE(single.optSecondary.has_value());
  }

  TEST_CASE("Decoder - parseSlashPair tolerates missing or invalid components", "[tag][unit][decoder]")
  {
    // Empty primary before the slash.
    auto const leadingSlash = parseSlashPair("/7");
    CHECK_FALSE(leadingSlash.optPrimary.has_value());
    REQUIRE(leadingSlash.optSecondary.has_value());
    CHECK(*leadingSlash.optSecondary == 7U);

    // Wholly empty and non-numeric inputs yield no values.
    CHECK_FALSE(parseSlashPair("").optPrimary.has_value());
    CHECK_FALSE(parseSlashPair("abc").optPrimary.has_value());

    // Out-of-range for uint16 is rejected (not silently truncated).
    CHECK_FALSE(parseSlashPair("70000").optPrimary.has_value());
  }
} // namespace ao::tag::test
