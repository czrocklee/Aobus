// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "AllTrackIdsList.h"

#include <algorithm>

namespace app::model
{

  AllTrackIdsList::AllTrackIdsList(rs::core::TrackStore& store)
    : _store{&store}
  {
  }

  void AllTrackIdsList::reloadFromStore(rs::lmdb::ReadTransaction& txn)
  {
    auto reader = _store->reader(txn);
    _trackIds.clear();
    _trackIds.reserve(1000); // Reserve initial capacity

    for (auto const& [id, view] : reader)
    {
      (void)view; // TrackView not needed, only TrackId
      _trackIds.push_back(id);
    }

    // Maintain ascending TrackId order
    std::sort(_trackIds.begin(), _trackIds.end());

    TrackIdList::notifyReset();
  }

  void AllTrackIdsList::notifyInserted(TrackId id)
  {
    // Insert in sorted position to maintain order
    auto const pos = std::lower_bound(_trackIds.begin(), _trackIds.end(), id);
    auto const index = static_cast<std::size_t>(std::distance(_trackIds.begin(), pos));
    _trackIds.insert(pos, id);

    TrackIdList::notifyInserted(id, index);
  }

  void AllTrackIdsList::notifyUpdated(TrackId id)
  {
    auto const optIndex = indexOf(id);
    if (optIndex.has_value())
    {
      TrackIdList::notifyUpdated(id, *optIndex);
    }
  }

  void AllTrackIdsList::notifyRemoved(TrackId id)
  {
    auto const optIndex = indexOf(id);
    if (optIndex.has_value())
    {
      auto const index = *optIndex;
      _trackIds.erase(_trackIds.begin() + static_cast<std::ptrdiff_t>(index));
      TrackIdList::notifyRemoved(id, index);
    }
  }

  void AllTrackIdsList::clear()
  {
    _trackIds.clear();
    TrackIdList::notifyReset();
  }

  std::optional<std::size_t> AllTrackIdsList::indexOf(TrackId id) const
  {
    auto const it = std::find(_trackIds.begin(), _trackIds.end(), id);
    if (it == _trackIds.end())
    {
      return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
  }

} // namespace app::model