// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/ProjectionAnchor.h"

#include <ao/CoreIds.h>
#include <ao/Exception.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <limits>
#include <optional>
#include <tuple>
#include <type_traits>
#include <variant>

namespace ao::rt
{
  namespace
  {
    std::size_t checkedRangeEnd(std::size_t const start, std::size_t const count)
    {
      if (count == 0 || count > std::numeric_limits<std::size_t>::max() - start)
      {
        throwException<Exception>("Projection anchor received an invalid row range");
      }

      return start + count;
    }
  } // namespace

  ProjectionAnchor ProjectionAnchor::bound(TrackId const trackId,
                                           std::size_t const anchorIndex,
                                           std::size_t const projectionSize)
  {
    if (anchorIndex >= projectionSize)
    {
      throwException<Exception>("Bound projection anchor must address a projected row");
    }

    return ProjectionAnchor{trackId, State::Bound, anchorIndex};
  }

  ProjectionAnchor ProjectionAnchor::gap(TrackId const trackId,
                                         std::size_t const anchorIndex,
                                         std::size_t const projectionSize)
  {
    return ProjectionAnchor{trackId, State::Gap, std::min(anchorIndex, projectionSize)};
  }

  ProjectionAnchor::ProjectionAnchor(TrackId const trackId, State const state, std::size_t const anchorIndex)
    : _trackId{trackId}, _state{state}, _anchorIndex{anchorIndex}
  {
    if (trackId == kInvalidTrackId)
    {
      throwException<Exception>("Projection anchor requires a valid track identity");
    }
  }

  void ProjectionAnchor::applyBatch(TrackListProjectionDeltaBatch const& batch,
                                    std::size_t const projectionSize,
                                    std::optional<std::size_t> const optTrackIndex)
  {
    if (batch.deltas.empty())
    {
      throwException<Exception>("Projection anchor requires a non-empty delta batch");
    }

    if (std::holds_alternative<ProjectionReset>(batch.deltas.front()))
    {
      if (batch.deltas.size() != 1)
      {
        throwException<Exception>("Projection reset must be the only delta in its batch");
      }

      reconcile(projectionSize, optTrackIndex);
      return;
    }

    for (auto const& delta : batch.deltas)
    {
      std::visit(
        [this](auto const& value)
        {
          using Value = std::remove_cvref_t<decltype(value)>;

          if constexpr (std::same_as<Value, ProjectionInsertRange>)
          {
            applyInsert(value.range.start, value.range.count);
          }
          else if constexpr (std::same_as<Value, ProjectionRemoveRange>)
          {
            applyRemove(value.range.start, value.range.count);
          }
          else if constexpr (std::same_as<Value, ProjectionUpdateRange>)
          {
            std::ignore = checkedRangeEnd(value.range.start, value.range.count);
          }
          else if constexpr (std::same_as<Value, ProjectionSourceInvalidated>)
          {
            throwException<Exception>("Projection anchor cannot consume a source-invalidated projection");
          }
          else
          {
            throwException<Exception>("Projection reset must be the only delta in its batch");
          }
        },
        delta);
    }

    reconcile(projectionSize, optTrackIndex);
  }

  void ProjectionAnchor::applyInsert(std::size_t const start, std::size_t const count)
  {
    std::ignore = checkedRangeEnd(start, count);
    auto const shiftsAnchor = _state == State::Bound ? start <= _anchorIndex : start < _anchorIndex;

    if (!shiftsAnchor)
    {
      return;
    }

    if (count > std::numeric_limits<std::size_t>::max() - _anchorIndex)
    {
      throwException<Exception>("Projection insertion overflows the anchor index");
    }

    _anchorIndex += count;
  }

  void ProjectionAnchor::applyRemove(std::size_t const start, std::size_t const count)
  {
    auto const end = checkedRangeEnd(start, count);

    if (end <= _anchorIndex)
    {
      _anchorIndex -= count;
      return;
    }

    if (start <= _anchorIndex && _anchorIndex < end)
    {
      _anchorIndex = start;

      if (_state == State::Bound)
      {
        _state = State::Gap;
      }
    }
  }

  void ProjectionAnchor::reconcile(std::size_t const projectionSize, std::optional<std::size_t> const optTrackIndex)
  {
    if (optTrackIndex)
    {
      if (*optTrackIndex >= projectionSize)
      {
        throwException<Exception>("Projection identity index is outside the final projection");
      }

      _state = State::Bound;
      _anchorIndex = *optTrackIndex;
      return;
    }

    _state = State::Gap;
    _anchorIndex = std::min(_anchorIndex, projectionSize);
  }
} // namespace ao::rt
