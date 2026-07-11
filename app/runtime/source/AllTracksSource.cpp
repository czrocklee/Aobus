// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "runtime/source/TrackSourceDeltaBuilder.h"
#include <ao/CoreIds.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/source/AllTracksSource.h>
#include <ao/rt/source/TrackSource.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::rt
{
  AllTracksSource::AllTracksSource(library::TrackStore& store)
    : _store{store}
  {
  }

  void AllTracksSource::reloadFromStore(lmdb::ReadTransaction const& transaction)
  {
    auto const reader = _store.reader(transaction);
    auto ids = std::vector<TrackId>{};
    ids.reserve(1000);

    for (auto const& [id, _] : reader)
    {
      ids.push_back(id);
    }

    std::ranges::sort(ids);
    ids.erase(std::ranges::unique(ids).begin(), ids.end());
    _trackIds = std::move(ids);

    TrackSource::notifyReset();
  }

  void AllTracksSource::notifyInserted(TrackId const id)
  {
    applyCollectionChange(std::span{&id, 1}, {});
  }

  void AllTracksSource::notifyRemoved(TrackId const id)
  {
    applyCollectionChange({}, std::span{&id, 1});
  }

  void AllTracksSource::applyCollectionChange(std::span<TrackId const> const inserted,
                                              std::span<TrackId const> const removed)
  {
    auto const previousSize = _trackIds.size();
    auto builder = TrackSourceDeltaBuilder{previousSize};
    auto removedIds = std::vector<TrackId>{};

    for (auto const id : removed)
    {
      if (auto const it = std::ranges::lower_bound(_trackIds, id);
          it != _trackIds.end() && *it == id && !std::ranges::contains(removedIds, id))
      {
        builder.remove(static_cast<std::size_t>(std::distance(_trackIds.begin(), it)), id);
        removedIds.push_back(id);
      }
    }

    for (auto const id : removedIds)
    {
      std::erase(_trackIds, id);
    }

    auto insertedIds = std::vector<TrackId>{};

    for (auto const id : inserted)
    {
      if (auto const it = std::ranges::lower_bound(_trackIds, id); it == _trackIds.end() || *it != id)
      {
        _trackIds.insert(it, id);
        insertedIds.push_back(id);
      }
    }

    for (auto const id : insertedIds)
    {
      auto const it = std::ranges::lower_bound(_trackIds, id);
      auto const index = static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
      builder.insert(index, id);
    }

    auto optBatch = builder.build();

    if (!optBatch)
    {
      return;
    }

    std::ignore = publishDeltaBatch(std::move(*optBatch), previousSize);
  }

  void AllTracksSource::clear()
  {
    if (_trackIds.empty())
    {
      return;
    }

    _trackIds.clear();
    TrackSource::notifyReset();
  }

  std::optional<std::size_t> AllTracksSource::indexOf(TrackId const id) const
  {
    if (auto const it = std::ranges::lower_bound(_trackIds, id); it != _trackIds.end() && *it == id)
    {
      return static_cast<std::size_t>(std::distance(_trackIds.begin(), it));
    }

    return std::nullopt;
  }
} // namespace ao::rt
