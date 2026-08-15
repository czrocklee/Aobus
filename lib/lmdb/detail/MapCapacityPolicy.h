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
   * move committed ones down. A file that can hold a hole is doubled, since the
   * unused remainder costs nothing; a file that cannot grows by
   * @c denseStepBytes, since there every added byte is an allocated byte.
   *
   * @param capacity   What the open environment reports about itself.
   * @param allocation What its map size costs on disk.
   * @param policy     Floor, ceiling, and dense growth step. A zero ceiling
   *                   forbids growth, so a default policy leaves the map alone.
   * @return Map size in bytes, at least @c capacity.mapBytes.
   */
  std::uint64_t plannedMapBytes(EnvironmentCapacity const& capacity,
                                MapAllocation allocation,
                                CapacityPolicy const& policy);
} // namespace ao::lmdb::detail
