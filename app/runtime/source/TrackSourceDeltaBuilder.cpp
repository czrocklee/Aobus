// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/source/TrackSourceDeltaBuilder.h"

#include <ao/CoreIds.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceEditScript.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
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

    for (std::size_t index = 0; index < removals.size(); ++index)
    {
      auto const& removal = removals[index];
      gsl_Assert(removal.index < _initialSize);
      gsl_Assert(index == 0 || removal.index != removals[index - 1].index);
    }

    for (std::size_t index = 1; index < insertions.size(); ++index)
    {
      gsl_Assert(insertions[index].index > insertions[index - 1].index);
    }

    auto coalescer = delta::Coalescer{};

    for (auto const& removal : removals | std::views::reverse)
    {
      coalescer.appendRemove(removal.index, std::span{&removal.trackId, 1});
    }

    for (auto const& insertion : insertions)
    {
      coalescer.appendInsert(insertion.index, std::span{&insertion.trackId, 1});
    }

    auto const script = coalescer.take();
    gsl_Assert(delta::validate(script, _initialSize));
    return sourceBatchOf(script);
  }
} // namespace ao::rt
