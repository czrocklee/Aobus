// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "runtime/source/AllTracksSource.h"

#include "runtime/source/TrackSourceDeltaBuilder.h"
#include <ao/CoreIds.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/TrackEditScript.h>
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
  namespace
  {
    std::vector<TrackId> sortedUniqueIds(std::span<TrackId const> const ids)
    {
      auto result = std::vector<TrackId>{ids.begin(), ids.end()};
      std::ranges::sort(result);
      result.erase(std::ranges::unique(result).begin(), result.end());
      return result;
    }

    std::optional<std::size_t> indexInSortedIds(std::span<TrackId const> const sortedIds, TrackId const id)
    {
      if (auto const it = std::ranges::lower_bound(sortedIds, id); it != sortedIds.end() && *it == id)
      {
        return static_cast<std::size_t>(std::distance(sortedIds.begin(), it));
      }

      return std::nullopt;
    }

    // Metadata-only changes keep the installed snapshot; each updated id
    // resolves by binary search in the sorted source, so publication stays
    // bounded by the changeset rather than by the source size.
    std::optional<delta::RegularTrackEditScript> buildMetadataOnlyUpdateBatch(std::span<TrackId const> const trackIds,
                                                                              std::span<TrackId const> const updated)
    {
      auto builder = TrackSourceDeltaBuilder{trackIds.size()};

      for (auto const id : sortedUniqueIds(updated))
      {
        if (auto const optIndex = indexInSortedIds(trackIds, id); optIndex)
        {
          builder.update(*optIndex, id);
        }
      }

      return builder.build();
    }
  } // namespace

  AllTracksSource::AllTracksSource(library::TrackStore const& store)
    : _store{store}
  {
  }

  void AllTracksSource::reloadFromStore(library::ReadTransaction const& transaction)
  {
    if (state() == TrackSourceState::Invalidated)
    {
      return;
    }

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

  void AllTracksSource::applyChanges(std::span<TrackId const> const inserted,
                                     std::span<TrackId const> const removed,
                                     std::span<TrackId const> const updated)
  {
    if (state() == TrackSourceState::Invalidated)
    {
      return;
    }

    auto const previousSize = _trackIds.size();

    if (inserted.empty() && removed.empty())
    {
      if (auto optBatch = buildMetadataOnlyUpdateBatch(_trackIds, updated); optBatch)
      {
        std::ignore = tryPublishDelta(std::move(*optBatch), previousSize);
      }

      return;
    }

    auto builder = TrackSourceDeltaBuilder{previousSize};
    auto const insertedIds = sortedUniqueIds(inserted);
    auto const removedIds = sortedUniqueIds(removed);

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

    for (auto const id : sortedUniqueIds(updated))
    {
      if (std::ranges::binary_search(insertedIds, id) || std::ranges::binary_search(removedIds, id))
      {
        continue;
      }

      if (auto const optIndex = indexInSortedIds(finalIds, id); optIndex)
      {
        builder.update(*optIndex, id);
      }
    }

    _trackIds = std::move(finalIds);

    auto optBatch = builder.build();

    if (!optBatch)
    {
      return;
    }

    std::ignore = tryPublishDelta(std::move(*optBatch), previousSize);
  }

  std::optional<std::size_t> AllTracksSource::indexOf(TrackId const id) const
  {
    return indexInSortedIds(_trackIds, id);
  }

  void AllTracksSource::discardSnapshot() noexcept
  {
    _trackIds.clear();
  }
} // namespace ao::rt
