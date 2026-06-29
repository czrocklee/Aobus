// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/source/TrackSource.h>

#include <cstddef>
#include <span>

namespace ao::rt
{
  TrackSource::~TrackSource()
  {
    for (auto* obs : _observers)
    {
      obs->onSourceDestroyed();
    }
  }

  void TrackSource::attach(TrackSourceObserver* observer)
  {
    _observers.push_back(observer);
  }

  void TrackSource::detach(TrackSourceObserver* observer)
  {
    std::erase(_observers, observer);
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
    for (auto* const obs : _observers)
    {
      obs->onBulkInserted(ids);
    }
  }

  void TrackSource::notifyUpdated(std::span<TrackId const> const ids)
  {
    for (auto* const obs : _observers)
    {
      obs->onBulkUpdated(ids);
    }
  }

  void TrackSource::notifyRemoved(std::span<TrackId const> const ids)
  {
    for (auto* const obs : _observers)
    {
      obs->onBulkRemoved(ids);
    }
  }

  void TrackSource::notifyReset()
  {
    for (auto* const obs : _observers)
    {
      obs->onReset();
    }
  }

  void TrackSource::notifyInserted(TrackId id, std::size_t index)
  {
    for (auto* const obs : _observers)
    {
      obs->onInserted(id, index);
    }
  }

  void TrackSource::notifyUpdated(TrackId id, std::size_t index)
  {
    for (auto* const obs : _observers)
    {
      obs->onUpdated(id, index);
    }
  }

  void TrackSource::notifyRemoved(TrackId id, std::size_t index)
  {
    for (auto* const obs : _observers)
    {
      obs->onRemoved(id, index);
    }
  }
} // namespace ao::rt
