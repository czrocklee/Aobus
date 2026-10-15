// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "MapCapacityPolicy.h"

#include <ao/Contract.h>
#include <ao/lmdb/Environment.h>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace ao::lmdb::detail
{
  std::uint64_t plannedMapBytes(EnvironmentCapacity const& capacity, CapacityPolicy const& policy)
  {
    AO_EXPECTS(policy.maximumMapBytes == 0 || policy.minimumMapBytes <= policy.maximumMapBytes,
               "LMDB capacity policy floor {} exceeds its ceiling {}",
               policy.minimumMapBytes,
               policy.maximumMapBytes);

    constexpr auto kMaxBytes = std::numeric_limits<std::uint64_t>::max();
    auto planned = std::max(capacity.mapBytes, policy.minimumMapBytes);
    // Headroom the map needs so the next mutations do not run it out: as much
    // again as the high water already covers.
    auto const required =
      capacity.highWaterBytes > kMaxBytes - capacity.highWaterBytes ? kMaxBytes : capacity.highWaterBytes * 2U;

    // The loop condition carries the ceiling, so a policy that forbids growth
    // never enters it and the floor above is the whole answer. An empty map has
    // nothing to double.
    while (planned < required && planned < policy.maximumMapBytes && planned > 0)
    {
      planned = std::min(planned > kMaxBytes - planned ? kMaxBytes : planned * 2U, policy.maximumMapBytes);
    }

    return planned;
  }
} // namespace ao::lmdb::detail
