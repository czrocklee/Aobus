// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/source/TrackSourceDelta.h>

#include <cstddef>
#include <optional>
#include <vector>

namespace ao::rt
{
  /**
   * Builds a source batch from identities supplied by a known operation.
   *
   * Removal indices address the source before the operation. Insertion indices
   * use the sequential coordinate space after every removal and any earlier
   * ascending insertion. Register consecutive inserted IDs at consecutive
   * indices so their resulting order is explicit.
   *
   * The builder never compares old and new source snapshots. Callers must
   * register every affected TrackId, and an operation with no registered edits
   * produces no batch.
   */
  class TrackSourceDeltaBuilder final
  {
  public:
    explicit TrackSourceDeltaBuilder(std::size_t initialSize) noexcept;

    void remove(std::size_t originalIndex, TrackId trackId);
    void insert(std::size_t postRemovalIndex, TrackId trackId);

    std::optional<TrackSourceDeltaBatch> build() const;

  private:
    struct IndexedTrack final
    {
      std::size_t index = 0;
      TrackId trackId{};
    };

    std::size_t _initialSize = 0;
    std::vector<IndexedTrack> _removals;
    std::vector<IndexedTrack> _insertions;
  };
} // namespace ao::rt
