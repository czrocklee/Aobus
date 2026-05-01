// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/model/AllTrackIdsList.h>

#include <algorithm>
#include <ranges>

namespace rs::model
{
  AllTrackIdsList::AllTrackIdsList(rs::library::TrackStore& store)
    : _store{store}
  {
  }

  void AllTrackIdsList::reloadFromStore(rs::lmdb::ReadTransaction& txn)
  {
    auto reader = _store.reader(txn);
    std::vector<TrackId> ids;
    ids.reserve(1000);

    for (auto const& [id, view] : reader)
    {
      (void)view;
      ids.push_back(id);
    }

    // flat_set constructor from sorted vector is efficient.
    std::ranges::sort(ids);
    _trackIds = std::flat_set<TrackId>(std::sorted_unique, std::move(ids));

    TrackIdList::notifyReset();
  }

  void AllTrackIdsList::notifyInserted(TrackId id)
  {
    if (auto [it, inserted] = _trackIds.insert(id); inserted)
    {
      auto const index = static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
      TrackIdList::notifyInserted(id, index);
    }
  }

  void AllTrackIdsList::notifyUpdated(TrackId id)
  {
    if (auto const optIndex = indexOf(id))
    {
      TrackIdList::notifyUpdated(id, *optIndex);
    }
  }

  void AllTrackIdsList::notifyRemoved(TrackId id)
  {
    if (auto const it = _trackIds.find(id); it != _trackIds.end())
    {
      auto const index = static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
      _trackIds.erase(it);
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
    if (auto const it = _trackIds.find(id); it != _trackIds.end())
    {
      return static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
    }

    return std::nullopt;
  }
} // namespace rs::model
