// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "AllTracksSource.h"

#include "TrackSource.h"

#include <ao/Type.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>

#include <algorithm>
#include <flat_set>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

#include <cstddef>

namespace ao::rt
{
  AllTracksSource::AllTracksSource(library::TrackStore& store)
    : _store{store}
  {
  }

  void AllTracksSource::reloadFromStore(lmdb::ReadTransaction const& txn)
  {
    auto const reader = _store.reader(txn);
    auto ids = std::vector<TrackId>{};
    ids.reserve(1000);

    for (auto const& [id, _] : reader)
    {
      ids.push_back(id);
    }

    // flat_set constructor from sorted vector is efficient.
    std::ranges::sort(ids);
    // NOLINTNEXTLINE(misc-include-cleaner)
    _trackIds = std::flat_set<TrackId>(std::sorted_unique, std::move(ids));

    TrackSource::notifyReset();
  }

  void AllTracksSource::notifyInserted(TrackId const id)
  {
    if (auto [it, inserted] = _trackIds.insert(id); inserted)
    {
      auto const index = static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
      TrackSource::notifyInserted(id, index);
    }
  }

  void AllTracksSource::notifyRemoved(TrackId const id)
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

  std::optional<std::size_t> AllTracksSource::indexOf(TrackId const id) const
  {
    if (auto const it = _trackIds.find(id); it != _trackIds.end())
    {
      return static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
    }

    return std::nullopt;
  }
}
