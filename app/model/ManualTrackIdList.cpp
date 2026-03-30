// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "ManualTrackIdList.h"

#include <algorithm>

namespace app::model
{

  ManualTrackIdList::ManualTrackIdList(rs::core::ListView const& view, TrackIdList* source)
    : _source{source}
  {
    if (_source != nullptr) { _source->attach(this); }
    reload(view);
  }

  ManualTrackIdList::ManualTrackIdList() = default;

  ManualTrackIdList::~ManualTrackIdList()
  {
    if (_source != nullptr) { _source->detach(this); }
  }

  void ManualTrackIdList::reload(rs::core::ListView const& view)
  {
    auto const trackIds = view.trackIds();
    _trackIds.assign(trackIds.begin(), trackIds.end());
    TrackIdList::notifyReset();
  }

  bool ManualTrackIdList::contains(TrackId id) const
  {
    return std::find(_trackIds.begin(), _trackIds.end(), id) != _trackIds.end();
  }

  void ManualTrackIdList::onReset()
  {
    // Remove any IDs from our list that are no longer in the source
    // For simplicity, just reload from stored data (manual lists don't auto-update membership)
    // but forward the reset notification so observers rebuild
    TrackIdList::notifyReset();
  }

  void ManualTrackIdList::onInserted(TrackId id, std::size_t /*index*/)
  {
    // Manual list doesn't add new tracks from source insert notifications
    // Just forward update notification if this track is in our list
    if (contains(id))
    {
      if (auto idx = indexOf(id)) { TrackIdList::notifyUpdated(id, *idx); }
    }
  }

  void ManualTrackIdList::onUpdated(TrackId id, std::size_t /*index*/)
  {
    // Forward update if this track is in our list
    if (contains(id))
    {
      if (auto idx = indexOf(id)) { TrackIdList::notifyUpdated(id, *idx); }
    }
  }

  void ManualTrackIdList::onRemoved(TrackId id, std::size_t /*index*/)
  {
    // If removed track is in our list, remove it and notify
    auto it = std::find(_trackIds.begin(), _trackIds.end(), id);
    if (it != _trackIds.end())
    {
      auto removeIdx = static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
      _trackIds.erase(it);
      TrackIdList::notifyRemoved(id, removeIdx);
    }
  }

  std::optional<std::size_t> ManualTrackIdList::indexOf(TrackId id) const
  {
    auto const it = std::find(_trackIds.begin(), _trackIds.end(), id);
    if (it == _trackIds.end()) { return std::nullopt; }
    return static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
  }

} // namespace app::model