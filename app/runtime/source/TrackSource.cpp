// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/Exception.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/rt/TrackEditScript.h>
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

  async::Subscription TrackSource::subscribe(std::move_only_function<void(TrackSourceDelta const&) noexcept> handler)
  {
    if (!handler)
    {
      throwException<Exception>("Track source subscription handler must not be empty");
    }

    if (_state == TrackSourceState::Invalidated)
    {
      handler(SourceInvalidated{});
      return {};
    }

    return _changedSignal.connect(std::move(handler));
  }

  void TrackSource::invalidate() noexcept
  {
    if (_state == TrackSourceState::Invalidated)
    {
      return;
    }

    _state = TrackSourceState::Invalidated;
    discardSnapshot();
    _changedSignal.emit(SourceInvalidated{});
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

    auto script = delta::RegularTrackEditScript{};
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

      if (!script.edits.empty())
      {
        auto& range = std::get<delta::InsertRange>(script.edits.back());

        if (range.start + range.trackIds.size() == index)
        {
          range.trackIds.push_back(trackId);
          continue;
        }
      }

      script.edits.emplace_back(delta::InsertRange{.start = index, .trackIds = {trackId}});
    }

    if (auto const currentSize = size(); !matchedIds.empty() && matchedIds.size() <= currentSize)
    {
      std::ignore = publishDelta(std::move(script), currentSize - matchedIds.size());
    }
  }

  void TrackSource::notifyUpdated(std::span<TrackId const> const ids)
  {
    if (ids.empty())
    {
      return;
    }

    auto script = delta::RegularTrackEditScript{};
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

      if (!script.edits.empty())
      {
        auto& range = std::get<delta::UpdateRange>(script.edits.back());

        if (range.start + range.trackIds.size() == index)
        {
          range.trackIds.push_back(trackId);
          continue;
        }
      }

      script.edits.emplace_back(delta::UpdateRange{.start = index, .trackIds = {trackId}});
    }

    if (!matchedIds.empty())
    {
      std::ignore = publishDelta(std::move(script), size());
    }
  }

  void TrackSource::notifyReset()
  {
    std::ignore = publishDelta(SourceReset{}, size());
  }

  void TrackSource::notifyInserted(TrackId id, std::size_t index)
  {
    if (auto const currentSize = size(); currentSize != 0)
    {
      std::ignore =
        publishDelta(delta::RegularTrackEditScript{.edits = {delta::InsertRange{.start = index, .trackIds = {id}}}},
                     currentSize - 1);
    }
  }

  void TrackSource::notifyUpdated(TrackId id, std::size_t index)
  {
    std::ignore = publishDelta(
      delta::RegularTrackEditScript{.edits = {delta::UpdateRange{.start = index, .trackIds = {id}}}}, size());
  }

  void TrackSource::notifyRemoved(TrackId id, std::size_t index)
  {
    if (auto const currentSize = size(); currentSize != std::numeric_limits<std::size_t>::max())
    {
      std::ignore =
        publishDelta(delta::RegularTrackEditScript{.edits = {delta::RemoveRange{.start = index, .trackIds = {id}}}},
                     currentSize + 1);
    }
  }

  bool TrackSource::publishDelta(TrackSourceDelta message, std::size_t const previousSize)
  {
    if (_state == TrackSourceState::Invalidated)
    {
      return false;
    }

    gsl_Assert(validateTrackSourceDelta(message, previousSize) && !std::holds_alternative<SourceInvalidated>(message));

    _changedSignal.emit(message);
    return true;
  }
} // namespace ao::rt
