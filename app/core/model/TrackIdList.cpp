// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/model/TrackIdList.h"

namespace app::core::model
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

  void TrackIdList::notifyUpdated(TrackId id, std::size_t index)
  {
    for (auto* obs : _observers)
    {
      obs->onUpdated(id, index);
    }
  }

  void TrackIdList::notifyTrackDataChanged(TrackId id)
  {
    if (auto index = indexOf(id))
    {
      notifyUpdated(id, *index);
    }
  }

  void TrackIdList::notifyRemoved(TrackId id, std::size_t index)
  {
    for (auto* obs : _observers)
    {
      obs->onRemoved(id, index);
    }
  }

} // namespace app::core::model