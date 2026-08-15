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
  namespace
  {
    /// Bytes to add for one growth step, or zero when the policy offers none.
    std::uint64_t growthStep(std::uint64_t const current,
                             MapAllocation const allocation,
                             CapacityPolicy const& policy) noexcept
    {
      return allocation == MapAllocation::OnDemand ? current : policy.denseStepBytes;
    }

    /**
     * Floor for this allocation mode.
     *
     * The two modes need separate figures because a floor above the high-water
     * mark is free where the remainder is a hole and is immediate disk use
     * where it is not. One shared floor would have to be wrong for one of them.
     */
    std::uint64_t floorMapBytes(MapAllocation const allocation, CapacityPolicy const& policy) noexcept
    {
      return allocation == MapAllocation::OnDemand ? policy.minimumMapBytes : policy.denseMinimumMapBytes;
    }

    /// Headroom the map needs so the next mutations do not run it out.
    std::uint64_t requiredMapBytes(EnvironmentCapacity const& capacity,
                                   MapAllocation const allocation,
                                   CapacityPolicy const& policy) noexcept
    {
      constexpr auto kMaxBytes = std::numeric_limits<std::uint64_t>::max();
      auto const reserve = growthStep(capacity.highWaterBytes, allocation, policy);
      return reserve > kMaxBytes - capacity.highWaterBytes ? kMaxBytes : capacity.highWaterBytes + reserve;
    }
  } // namespace

  std::uint64_t plannedMapBytes(EnvironmentCapacity const& capacity,
                                MapAllocation const allocation,
                                CapacityPolicy const& policy)
  {
    AO_EXPECTS(policy.maximumMapBytes == 0 || policy.minimumMapBytes <= policy.maximumMapBytes,
               "LMDB capacity policy floor {} exceeds its ceiling {}",
               policy.minimumMapBytes,
               policy.maximumMapBytes);
    AO_EXPECTS(policy.maximumMapBytes == 0 || policy.denseMinimumMapBytes <= policy.maximumMapBytes,
               "LMDB capacity policy dense floor {} exceeds its ceiling {}",
               policy.denseMinimumMapBytes,
               policy.maximumMapBytes);

    constexpr auto kMaxBytes = std::numeric_limits<std::uint64_t>::max();
    auto planned = std::max(capacity.mapBytes, floorMapBytes(allocation, policy));
    auto const required = requiredMapBytes(capacity, allocation, policy);

    // The loop condition carries the ceiling, so a policy that forbids growth
    // never enters it and the floor above is the whole answer.
    while (planned < required && planned < policy.maximumMapBytes)
    {
      auto const step = growthStep(planned, allocation, policy);

      if (step == 0)
      {
        // A dense file with no configured step has nothing to grow by.
        break;
      }

      planned = std::min(step > kMaxBytes - planned ? kMaxBytes : planned + step, policy.maximumMapBytes);
    }

    return planned;
  }
} // namespace ao::lmdb::detail
