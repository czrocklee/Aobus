// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/source/TrackSourceDeltaBuilder.h"

#include <ao/CoreIds.h>
#include <ao/Exception.h>
#include <ao/rt/source/TrackSourceDelta.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace ao::rt
{
  TrackSourceDeltaBuilder::TrackSourceDeltaBuilder(std::size_t const initialSize) noexcept
    : _initialSize{initialSize}
  {
  }

  void TrackSourceDeltaBuilder::remove(std::size_t const originalIndex, TrackId const trackId)
  {
    _removals.push_back(IndexedTrack{.index = originalIndex, .trackId = trackId});
  }

  void TrackSourceDeltaBuilder::insert(std::size_t const postRemovalIndex, TrackId const trackId)
  {
    _insertions.push_back(IndexedTrack{.index = postRemovalIndex, .trackId = trackId});
  }

  std::optional<TrackSourceDeltaBatch> TrackSourceDeltaBuilder::build() const
  {
    if (_removals.empty() && _insertions.empty())
    {
      return std::nullopt;
    }

    auto removals = _removals;
    auto insertions = _insertions;
    std::ranges::sort(removals, {}, &IndexedTrack::index);
    std::ranges::sort(insertions, {}, &IndexedTrack::index);

    auto removalRanges = std::vector<SourceRemoveRange>{};
    removalRanges.reserve(removals.size());

    for (auto const& removal : removals)
    {
      if (removal.index >= _initialSize)
      {
        throwException<Exception>("Track source removal index is outside the pre-operation source");
      }

      if (!removalRanges.empty())
      {
        auto& previous = removalRanges.back();
        auto const previousEnd = previous.start + previous.trackIds.size();

        if (removal.index == previous.start || removal.index < previousEnd)
        {
          throwException<Exception>("Track source removal indices overlap");
        }

        if (removal.index == previousEnd)
        {
          previous.trackIds.push_back(removal.trackId);
          continue;
        }
      }

      removalRanges.push_back(SourceRemoveRange{.start = removal.index, .trackIds = {removal.trackId}});
    }

    auto insertionRanges = std::vector<SourceInsertRange>{};
    insertionRanges.reserve(insertions.size());

    for (auto const& insertion : insertions)
    {
      if (!insertionRanges.empty())
      {
        auto& previous = insertionRanges.back();
        auto const previousEnd = previous.start + previous.trackIds.size();

        if (insertion.index < previousEnd)
        {
          throwException<Exception>("Track source insertion indices overlap");
        }

        if (insertion.index == previousEnd)
        {
          previous.trackIds.push_back(insertion.trackId);
          continue;
        }
      }

      insertionRanges.push_back(SourceInsertRange{.start = insertion.index, .trackIds = {insertion.trackId}});
    }

    auto batch = TrackSourceDeltaBatch{};
    batch.deltas.reserve(removalRanges.size() + insertionRanges.size());

    for (auto& removal : removalRanges | std::views::reverse)
    {
      batch.deltas.push_back(std::move(removal));
    }

    for (auto& insertion : insertionRanges)
    {
      batch.deltas.push_back(std::move(insertion));
    }

    if (!validateTrackSourceDeltaBatch(batch, _initialSize))
    {
      throwException<Exception>("Track source operation produced invalid sequential delta coordinates");
    }

    return batch;
  }
} // namespace ao::rt
