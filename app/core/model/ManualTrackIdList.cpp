// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "core/model/ManualTrackIdList.h"

#include <rs/core/ListView.h>

#include <algorithm>

namespace app::core::model
{

  ManualTrackIdList::ManualTrackIdList(rs::core::ListView const& view, TrackIdList* source)
    : _source{source}
  {
    _trackIds.reserve(view.tracks().size());
    for (auto const& id : view.tracks())
    {
      _trackIds.push_back(id);
    }

    if (_source)
    {
      _source->attach(this);
    }
  }

  ManualTrackIdList::ManualTrackIdList() = default;

  ManualTrackIdList::~ManualTrackIdList()
  {
    if (_source)
    {
      _source->detach(this);
    }
  }

  void ManualTrackIdList::reloadFromListView(rs::core::ListView const& view)
  {
    _trackIds.clear();
    _trackIds.reserve(view.tracks().size());
    for (auto const& id : view.tracks())
    {
      if (!_source || _source->indexOf(id))
      {
        _trackIds.push_back(id);
      }
    }
    TrackIdList::notifyReset();
  }

  void ManualTrackIdList::onReset()
  {
    if (!_source)
    {
      return;
    }

    // Filter existing members against new source state
    std::vector<TrackId> next;
    for (auto id : _trackIds)
    {
      if (_source->indexOf(id))
      {
        next.push_back(id);
      }
    }
    _trackIds = std::move(next);
    TrackIdList::notifyReset();
  }

  void ManualTrackIdList::onInserted(TrackId id, std::size_t /*index*/)
  {
    (void)id;
  }

  void ManualTrackIdList::onUpdated(TrackId id, std::size_t /*index*/)
  {
    if (auto const myIndex = indexOf(id))
    {
      TrackIdList::notifyUpdated(id, *myIndex);
    }
  }

  void ManualTrackIdList::onRemoved(TrackId id, std::size_t /*index*/)
  {
    auto it = std::find(_trackIds.begin(), _trackIds.end(), id);
    
    if (it != _trackIds.end())
    {
      auto const myIndex = static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
      _trackIds.erase(it);
      TrackIdList::notifyRemoved(id, myIndex);
    }
  }

  void ManualTrackIdList::onBatchInserted(std::span<TrackId const> /*ids*/)
  {
  }

  void ManualTrackIdList::onBatchUpdated(std::span<TrackId const> ids)
  {
    std::vector<TrackId> matched;
    for (auto id : ids)
    {
      if (contains(id))
      {
        matched.push_back(id);
      }
    }
    if (!matched.empty())
    {
      TrackIdList::notifyBatchUpdated(matched);
    }
  }

  void ManualTrackIdList::onBatchRemoved(std::span<TrackId const> ids)
  {
    std::vector<TrackId> removed;
    for (auto id : ids)
    {
      auto it = std::find(_trackIds.begin(), _trackIds.end(), id);
      
      if (it != _trackIds.end())
      {
        _trackIds.erase(it);
        removed.push_back(id);
      }
    }
    if (!removed.empty())
    {
      TrackIdList::notifyBatchRemoved(removed);
    }
  }

  bool ManualTrackIdList::contains(TrackId id) const
  {
    return std::find(_trackIds.begin(), _trackIds.end(), id) != _trackIds.end();
  }

  std::optional<std::size_t> ManualTrackIdList::indexOf(TrackId id) const
  {
    auto const it = std::find(_trackIds.begin(), _trackIds.end(), id);
    
    if (it == _trackIds.end())
    {
      return std::nullopt;
    }
    
    return static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
  }

} // namespace app::core::model
