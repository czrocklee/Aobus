// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/model/TrackIdList.h>

#include <algorithm>

namespace rs::model
{
  TrackIdList::~TrackIdList()
  {
    for (auto* obs : _observers)
    {
      obs->onSourceDestroyed();
    }
  }

  void TrackIdList::attach(TrackIdListObserver* observer)
  {
    _observers.push_back(observer);
  }

  void TrackIdList::detach(TrackIdListObserver* observer)
  {
    std::erase(_observers, observer);
  }

  void TrackIdList::notifyUpdated(TrackId id)
  {
    if (auto const index = indexOf(id))
    {
      notifyUpdated(id, *index);
    }
  }

  void TrackIdList::notifyUpdated(std::span<TrackId const> ids)
  {
    for (auto* obs : _observers)
    {
      obs->onUpdated(ids);
    }
  }

  void TrackIdList::notifyReset()
  {
    for (auto* obs : _observers)
    {
      obs->onReset();
    }
  }

  void TrackIdList::notifyInserted(TrackId id, std::size_t index)
  {
    for (auto* obs : _observers)
    {
      obs->onInserted(id, index);
    }
  }

  void TrackIdList::notifyInserted(std::span<TrackId const> ids)
  {
    for (auto* obs : _observers)
    {
      obs->onInserted(ids);
    }
  }

  void TrackIdList::notifyUpdated(TrackId id, std::size_t index)
  {
    for (auto* obs : _observers)
    {
      obs->onUpdated(id, index);
    }
  }

  void TrackIdList::notifyRemoved(TrackId id, std::size_t index)
  {
    for (auto* obs : _observers)
    {
      obs->onRemoved(id, index);
    }
  }

  void TrackIdList::notifyRemoved(std::span<TrackId const> ids)
  {
    for (auto* obs : _observers)
    {
      obs->onRemoved(ids);
    }
  }
} // namespace rs::model
