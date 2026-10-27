// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "lib/lmdb/detail/MapCapacityPolicy.h"

#include <ao/lmdb/Environment.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

namespace ao::lmdb::test
{
  namespace
  {
    constexpr std::uint64_t mebibytes(std::uint64_t const count)
    {
      return count * 1024 * 1024;
    }

    constexpr EnvironmentCapacity capacityOf(std::uint64_t const mapBytes, std::uint64_t const highWaterBytes)
    {
      return EnvironmentCapacity{.mapBytes = mapBytes, .highWaterBytes = highWaterBytes, .pageBytes = 4096};
    }

    constexpr CapacityPolicy growingPolicy()
    {
      return CapacityPolicy{.minimumMapBytes = mebibytes(16), .maximumMapBytes = mebibytes(1024)};
    }
  } // namespace

  TEST_CASE("plannedMapBytes - a default policy leaves the map alone", "[lmdb][unit][capacity]")
  {
    auto const capacity = capacityOf(mebibytes(8), mebibytes(7));

    // Nearly full, but a policy that names no ceiling has not asked for growth.
    CHECK(detail::plannedMapBytes(capacity, CapacityPolicy{}) == mebibytes(8));
  }

  TEST_CASE("plannedMapBytes - the floor lifts a map below it", "[lmdb][unit][capacity]")
  {
    // What LMDB gives a database that never configured a size.
    auto const capacity = capacityOf(mebibytes(1), mebibytes(1));

    CHECK(detail::plannedMapBytes(capacity, growingPolicy()) == mebibytes(16));
  }

  TEST_CASE("plannedMapBytes - a map above the floor is never lowered to it", "[lmdb][unit][capacity]")
  {
    // The case that decides whether a database keeps capacity an earlier session
    // gave it. Lowering here would hand back less room than it already holds.
    auto const capacity = capacityOf(mebibytes(512), mebibytes(4));

    CHECK(detail::plannedMapBytes(capacity, growingPolicy()) == mebibytes(512));
  }

  TEST_CASE("plannedMapBytes - a map is not raised while the peak leaves half of it free", "[lmdb][unit][capacity]")
  {
    auto const capacity = capacityOf(mebibytes(64), mebibytes(32));

    CHECK(detail::plannedMapBytes(capacity, growingPolicy()) == mebibytes(64));
  }

  TEST_CASE("plannedMapBytes - a map doubles once the peak passes half", "[lmdb][unit][capacity]")
  {
    auto const capacity = capacityOf(mebibytes(64), mebibytes(33));

    CHECK(detail::plannedMapBytes(capacity, growingPolicy()) == mebibytes(128));
  }

  TEST_CASE("plannedMapBytes - doubling repeats until the peak has room", "[lmdb][unit][capacity]")
  {
    // A peak far past the map takes several doublings, which is what lets one
    // open recover a database that grew while its map did not.
    auto const capacity = capacityOf(mebibytes(32), mebibytes(200));

    CHECK(detail::plannedMapBytes(capacity, growingPolicy()) == mebibytes(512));
  }

  TEST_CASE("plannedMapBytes - the ceiling stops growth", "[lmdb][unit][capacity]")
  {
    auto const capacity = capacityOf(mebibytes(512), mebibytes(500));

    // Doubling would ask for 1024 MiB, which is the ceiling, and the next step
    // would pass it.
    CHECK(detail::plannedMapBytes(capacity, growingPolicy()) == mebibytes(1024));

    auto const atCeiling = capacityOf(mebibytes(1024), mebibytes(1000));
    CHECK(detail::plannedMapBytes(atCeiling, growingPolicy()) == mebibytes(1024));
  }

  TEST_CASE("plannedMapBytes - a map already past the ceiling is left where it is", "[lmdb][unit][capacity]")
  {
    // A database opened under a larger ceiling keeps its map: growth is bounded,
    // but nothing shrinks a map that already exists.
    auto const capacity = capacityOf(mebibytes(4096), mebibytes(4000));

    CHECK(detail::plannedMapBytes(capacity, growingPolicy()) == mebibytes(4096));
  }

  TEST_CASE("plannedMapBytes - a policy with no ceiling still honours its floor", "[lmdb][unit][capacity]")
  {
    // A ceiling of zero disables the growth rule, not the floor. The two answer
    // different questions: the floor is the capacity the caller asked to open
    // with, while growth reacts to how full the database has become. The peak
    // here is far past the map, so any growth at all would overshoot the floor.
    auto const capacity = capacityOf(mebibytes(8), mebibytes(900));
    auto const policy = CapacityPolicy{.minimumMapBytes = mebibytes(16), .maximumMapBytes = 0};

    CHECK(detail::plannedMapBytes(capacity, policy) == mebibytes(16));
  }

  TEST_CASE("plannedMapBytes - a peak near the representable limit does not wrap", "[lmdb][unit][capacity]")
  {
    constexpr auto kMaxBytes = std::numeric_limits<std::uint64_t>::max();
    auto const capacity = capacityOf(kMaxBytes / 2, kMaxBytes - 1024);
    auto const policy = CapacityPolicy{.minimumMapBytes = mebibytes(16), .maximumMapBytes = kMaxBytes};

    // Doubling the map and reserving twice the peak both overflow, and the answer
    // has to saturate at the ceiling rather than come back small.
    CHECK(detail::plannedMapBytes(capacity, policy) == kMaxBytes);
  }
} // namespace ao::lmdb::test
