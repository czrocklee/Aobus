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
    auto insertedIds = std::vector<TrackId>{inserted.begin(), inserted.end()};
    auto removedIds = std::vector<TrackId>{removed.begin(), removed.end()};
    std::ranges::sort(insertedIds);
    std::ranges::sort(removedIds);
    insertedIds.erase(std::ranges::unique(insertedIds).begin(), insertedIds.end());
    removedIds.erase(std::ranges::unique(removedIds).begin(), removedIds.end());

    auto retained = std::vector<TrackId>{};
    retained.reserve(_trackIds.size());
    std::size_t removedIndex = 0;

    for (std::size_t index = 0; index < _trackIds.size(); ++index)
    {
      auto const id = _trackIds[index];

      while (removedIndex < removedIds.size() && removedIds[removedIndex] < id)
      {
        ++removedIndex;
      }

      if (removedIndex < removedIds.size() && removedIds[removedIndex] == id)
      {
        builder.remove(index, id);
        ++removedIndex;
      }
      else
      {
        retained.push_back(id);
      }
    }

    auto finalIds = std::vector<TrackId>{};
    finalIds.reserve(retained.size() + insertedIds.size());
    std::size_t retainedIndex = 0;
    std::size_t insertedIndex = 0;

    while (retainedIndex < retained.size() || insertedIndex < insertedIds.size())
    {
      if (insertedIndex == insertedIds.size() ||
          (retainedIndex < retained.size() && retained[retainedIndex] < insertedIds[insertedIndex]))
      {
        finalIds.push_back(retained[retainedIndex++]);
      }
      else if (retainedIndex < retained.size() && retained[retainedIndex] == insertedIds[insertedIndex])
      {
        finalIds.push_back(retained[retainedIndex++]);
        ++insertedIndex;
      }
      else
      {
        auto const id = insertedIds[insertedIndex++];
        builder.insert(finalIds.size(), id);
        finalIds.push_back(id);
      }
    }

    _trackIds = std::move(finalIds);

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
