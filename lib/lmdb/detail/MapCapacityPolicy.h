// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/lmdb/Environment.h>

#include <cstdint>

namespace ao::lmdb::detail
{
  /**
   * @brief Decides the map size an environment should hold once it is open.
   *
   * The rule is grow-only: the answer is never below what the environment
   * already maps, so a database that was opened once with a large map keeps it
   * even under a policy that would have chosen less.
   *
   * Headroom is measured against the high water rather than live data, because
   * that is what a mutation runs out of: LMDB reuses freed pages but does not
   * move committed ones down. The map doubles, since the data file grows with
   * the pages transactions use and the unused remainder costs no disk.
   *
   * @param capacity What the open environment reports about itself.
   * @param policy   Floor and ceiling. A zero ceiling forbids growth, so a
   *                 default policy leaves the map alone.
   * @return Map size in bytes, at least @c capacity.mapBytes.
   */
  std::uint64_t plannedMapBytes(EnvironmentCapacity const& capacity, CapacityPolicy const& policy);
} // namespace ao::lmdb::detail
