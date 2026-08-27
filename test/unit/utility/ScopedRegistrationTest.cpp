// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace ao::utility::test
{
  TEST_CASE("ScopedRegistration - reset and destruction invoke a registration exactly once",
            "[utility][unit][scoped-registration]")
  {
    std::int32_t releaseCount = 0;

    {
      auto registration = ScopedRegistration{[&releaseCount] { ++releaseCount; }};
      REQUIRE(registration);

      registration.reset();
      CHECK_FALSE(registration);
      CHECK(releaseCount == 1);

      registration.reset();
      CHECK(releaseCount == 1);
    }

    CHECK(releaseCount == 1);
  }

  TEST_CASE("ScopedRegistration - moves transfer ownership and release replaced registrations",
            "[utility][unit][scoped-registration]")
  {
    std::int32_t firstReleaseCount = 0;
    std::int32_t secondReleaseCount = 0;

    {
      auto first = ScopedRegistration{[&firstReleaseCount] { ++firstReleaseCount; }};
      auto transferred = ScopedRegistration{std::move(first)};

      CHECK_FALSE(first);
      REQUIRE(transferred);

      auto second = ScopedRegistration{[&secondReleaseCount] { ++secondReleaseCount; }};
      second = std::move(transferred);

      CHECK(secondReleaseCount == 1);
      CHECK_FALSE(transferred);
      REQUIRE(second);
    }

    CHECK(firstReleaseCount == 1);
    CHECK(secondReleaseCount == 1);
  }

  TEST_CASE("ScopedRegistration - stack unwinding releases a registration",
            "[utility][regression][scoped-registration]")
  {
    std::int32_t releaseCount = 0;

    CHECK_THROWS_AS(
      [&]
      {
        auto registration = ScopedRegistration{[&releaseCount] { ++releaseCount; }};
        throw std::runtime_error{"construction failed"};
      }(),
      std::runtime_error);

    CHECK(releaseCount == 1);
  }
} // namespace ao::utility::test
