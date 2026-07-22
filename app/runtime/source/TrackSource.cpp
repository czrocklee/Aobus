// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Exception.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>

#include <gsl-lite/gsl-lite.hpp>

#include <cstddef>
#include <functional>
#include <limits>
#include <span>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace ao::rt
{
  TrackSource::~TrackSource() = default;

  async::Subscription TrackSource::subscribe(std::move_only_function<void(TrackSourceDeltaBatch const&)> handler)
  {
    if (!handler)
    {
      throwException<Exception>("Track source subscription handler must not be empty");
    }

    if (_state == TrackSourceState::Invalidated)
    {
      handler(TrackSourceDeltaBatch{.deltas = {SourceInvalidated{}}});
      return {};
    }

    return _changedSignal.connect(std::move(handler));
  }

  void TrackSource::invalidate()
  {
    if (_state == TrackSourceState::Invalidated)
    {
      return;
    }

    _state = TrackSourceState::Invalidated;
    _changedSignal.emit(TrackSourceDeltaBatch{.deltas = {SourceInvalidated{}}});
    _changedSignal.disconnectAll();
  }

  void TrackSource::notifyUpdated(TrackId id)
  {
    if (auto const optIndex = indexOf(id); optIndex)
    {
      notifyUpdated(id, *optIndex);
    }
  }

  void TrackSource::notifyInserted(std::span<TrackId const> const ids)
  {
    if (ids.empty())
    {
      return;
    }

    auto batch = TrackSourceDeltaBatch{};
    auto matchedIds = std::vector<TrackId>{};

    for (std::size_t index = 0; index < size(); ++index)
    {
      auto const trackId = trackIdAt(index);
      bool matched = false;

      for (auto const requestedId : ids)
      {
        if (trackId == requestedId)
        {
          matched = true;
          break;
        }
      }

      if (!matched)
      {
        continue;
      }

      matchedIds.push_back(trackId);

      if (!batch.deltas.empty())
      {
        auto& range = std::get<SourceInsertRange>(batch.deltas.back());

        if (range.start + range.trackIds.size() == index)
        {
          range.trackIds.push_back(trackId);
          continue;
        }
      }

      batch.deltas.push_back(SourceInsertRange{.start = index, .trackIds = {trackId}});
    }

    if (auto const currentSize = size(); !matchedIds.empty() && matchedIds.size() <= currentSize)
    {
      std::ignore = publishDeltaBatch(std::move(batch), currentSize - matchedIds.size());
    }
  }

  void TrackSource::notifyUpdated(std::span<TrackId const> const ids)
  {
    if (ids.empty())
    {
      return;
    }

    auto batch = TrackSourceDeltaBatch{};
    auto matchedIds = std::vector<TrackId>{};

    for (std::size_t index = 0; index < size(); ++index)
    {
      auto const trackId = trackIdAt(index);
      bool matched = false;

      for (auto const requestedId : ids)
      {
        if (trackId == requestedId)
        {
          matched = true;
          break;
        }
      }

      if (!matched)
      {
        continue;
      }

      matchedIds.push_back(trackId);

      if (!batch.deltas.empty())
      {
        auto& range = std::get<SourceUpdateRange>(batch.deltas.back());

        if (range.start + range.trackIds.size() == index)
        {
          range.trackIds.push_back(trackId);
          continue;
        }
      }

      batch.deltas.push_back(SourceUpdateRange{.start = index, .trackIds = {trackId}});
    }

    if (!matchedIds.empty())
    {
      std::ignore = publishDeltaBatch(std::move(batch), size());
    }
  }

  void TrackSource::notifyReset()
  {
    std::ignore = publishDeltaBatch(TrackSourceDeltaBatch{.deltas = {SourceReset{}}}, size());
  }

  void TrackSource::notifyInserted(TrackId id, std::size_t index)
  {
    if (auto const currentSize = size(); currentSize != 0)
    {
      std::ignore = publishDeltaBatch(
        TrackSourceDeltaBatch{.deltas = {SourceInsertRange{.start = index, .trackIds = {id}}}}, currentSize - 1);
    }
  }

  void TrackSource::notifyUpdated(TrackId id, std::size_t index)
  {
    std::ignore =
      publishDeltaBatch(TrackSourceDeltaBatch{.deltas = {SourceUpdateRange{.start = index, .trackIds = {id}}}}, size());
  }

  void TrackSource::notifyRemoved(TrackId id, std::size_t index)
  {
    if (auto const currentSize = size(); currentSize != std::numeric_limits<std::size_t>::max())
    {
      std::ignore = publishDeltaBatch(
        TrackSourceDeltaBatch{.deltas = {SourceRemoveRange{.start = index, .trackIds = {id}}}}, currentSize + 1);
    }
  }

  bool TrackSource::publishDeltaBatch(TrackSourceDeltaBatch batch, std::size_t const previousSize)
  {
    if (_state == TrackSourceState::Invalidated)
    {
      return false;
    }

    gsl_Assert(!batch.deltas.empty() && validateTrackSourceDeltaBatch(batch, previousSize) &&
               !std::holds_alternative<SourceInvalidated>(batch.deltas.front()));

    _changedSignal.emit(batch);
    return true;
  }
} // namespace ao::rt
