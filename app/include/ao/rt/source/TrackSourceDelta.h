// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <boost/container/small_vector.hpp>

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace ao::rt
{
  struct SourceInsertRange final
  {
    std::size_t start = 0;
    std::vector<TrackId> trackIds{};
  };

  struct SourceRemoveRange final
  {
    std::size_t start = 0;
    std::vector<TrackId> trackIds{};
  };

  struct SourceUpdateRange final
  {
    std::size_t start = 0;
    std::vector<TrackId> trackIds{};
  };

  struct SourceReset final
  {};

  struct SourceInvalidated final
  {};

  using TrackSourceDelta =
    std::variant<SourceInsertRange, SourceRemoveRange, SourceUpdateRange, SourceReset, SourceInvalidated>;

  struct TrackSourceDeltaBatch final
  {
    std::uint64_t revision = 0;
    boost::container::small_vector<TrackSourceDelta, 1> deltas{};
  };

  /**
   * Validates delta coordinates against a source of initialSize rows.
   *
   * Reset and Invalidated are valid only as singleton batches. Regular ranges
   * must be non-empty and fit the coordinate space produced by every preceding
   * delta in the batch.
   */
  bool validateTrackSourceDeltaBatch(TrackSourceDeltaBatch const& batch, std::size_t initialSize);
} // namespace ao::rt
