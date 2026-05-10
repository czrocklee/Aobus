// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "AllTracksSource.h"

#include <algorithm>
#include <ranges>

namespace ao::app
{
  AllTracksSource::AllTracksSource(ao::library::TrackStore& store)
    : _store{store}
  {
  }

  void AllTracksSource::reloadFromStore(ao::lmdb::ReadTransaction& txn)
  {
    auto reader = _store.reader(txn);
    auto ids = std::vector<TrackId>{};
    ids.reserve(1000);

    for (auto const& [id, _] : reader)
    {
      ids.push_back(id);
    }

    // flat_set constructor from sorted vector is efficient.
    std::ranges::sort(ids);
    _trackIds = std::flat_set<TrackId>(std::sorted_unique, std::move(ids));

    TrackSource::notifyReset();
  }

  void AllTracksSource::notifyInserted(TrackId id)
  {
    if (auto [it, inserted] = _trackIds.insert(id); inserted)
    {
      auto const index = static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
      TrackSource::notifyInserted(id, index);
    }
  }

  void AllTracksSource::notifyRemoved(TrackId id)
  {
    if (auto const it = _trackIds.find(id); it != _trackIds.end())
    {
      auto const index = static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
      _trackIds.erase(it);
      TrackSource::notifyRemoved(id, index);
    }
  }

  void AllTracksSource::clear()
  {
    _trackIds.clear();
    TrackSource::notifyReset();
  }

  std::optional<std::size_t> AllTracksSource::indexOf(TrackId id) const
  {
    if (auto const it = _trackIds.find(id); it != _trackIds.end())
    {
      return static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
    }

    return std::nullopt;
  }
}
