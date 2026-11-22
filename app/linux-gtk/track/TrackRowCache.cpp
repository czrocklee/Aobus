// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "track/TrackRowCache.h"

#include "track/TrackRowObject.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/projection/TrackListProjection.h>

#include <glibmm/refptr.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace ao::gtk
{
  TrackRowCache::TrackRowCache(rt::Library const& reads, i18n::MessageCatalog textCatalog)
    : _reads{reads}, _textCatalog{std::move(textCatalog)}
  {
    _changesSub = _reads.changes().onChanged(
      [this](rt::LibraryChangeSet const& changeSet)
      {
        _observedRevision = changeSet.libraryRevision;
        auto bound = std::vector<TrackId>{};

        if (changeSet.libraryReset)
        {
          for (auto& pair : _rowCache)
          {
            markStale(pair.second, _observedRevision, bound);
          }
        }
        else
        {
          for (auto const trackId : changeSet.tracksDeleted)
          {
            _rowCache.erase(trackId);
          }

          auto const mark = [&](TrackId id) { markCachedRow(id, _observedRevision, bound); };
          std::ranges::for_each(changeSet.tracksInserted, mark);
          std::ranges::for_each(changeSet.tracksMutated, mark);
        }

        for (auto const id : bound)
        {
          trackRow(id);
        }

        if (changeSet.libraryReset)
        {
          // A list-only Restore also publishes a reset. Keep bindings that
          // already resolved surviving tracks in the new revision.
          boost::unordered::erase_if(
            _rowCache, [this](auto const& pair) { return pair.second.loadedRevision < _observedRevision; });
        }
      });
  }

  Glib::RefPtr<TrackRowObject> TrackRowCache::trackRow(TrackId id) const
  {
    auto it = _rowCache.find(id);

    if (it != _rowCache.end() && !it->second.stale)
    {
      return it->second.rowPtr;
    }

    auto optRow = std::optional<rt::TrackRow>{};
    {
      auto scope = _reads.snapshot();
      auto const revision = scope.revision();

      ++_rowLoadCount;
      optRow = scope.trackRow(id);

      if (!optRow)
      {
        _rowCache.erase(id);
        return nullptr;
      }

      if (it == _rowCache.end())
      {
        it = _rowCache.emplace(id, Entry{.rowPtr = TrackRowObject::create(id, _textCatalog)}).first;
      }

      it->second.loadedRevision = revision;
      it->second.stale = false;
    }

    // No transaction or map entry is borrowed by the cell callbacks fired by populate().
    auto const rowPtr = it->second.rowPtr;
    rowPtr->populate(std::move(*optRow));
    return rowPtr;
  }

  void TrackRowCache::markStale(Entry& entry, std::uint64_t revision, std::vector<TrackId>& bound)
  {
    if (entry.loadedRevision >= revision)
    {
      return;
    }

    entry.stale = true;

    if (!entry.rowPtr->signalFieldsChanged().empty())
    {
      bound.push_back(entry.rowPtr->trackId());
    }
  }

  void TrackRowCache::markCachedRow(TrackId id, std::uint64_t revision, std::vector<TrackId>& bound) const
  {
    if (auto const it = _rowCache.find(id); it != _rowCache.end())
    {
      markStale(it->second, revision, bound);
    }
  }

  void TrackRowCache::markCachedRange(rt::TrackListProjection const& projection,
                                      rt::TrackRowRange range,
                                      std::uint64_t revision,
                                      std::vector<TrackId>& bound) const
  {
    // Walk the smaller side: a large insertion or reset may cover very few
    // cached rows, while a single edit may hit a large warm cache.
    if (range.count <= _rowCache.size())
    {
      for (std::size_t offset = 0; offset < range.count; ++offset)
      {
        markCachedRow(projection.trackIdAt(range.start + offset), revision, bound);
      }

      return;
    }

    for (auto& [id, entry] : _rowCache)
    {
      if (auto const optIndex = projection.indexOf(id);
          optIndex && range.start <= *optIndex && *optIndex - range.start < range.count)
      {
        markStale(entry, revision, bound);
      }
    }
  }

  void TrackRowCache::prepareNotification(rt::TrackListProjection const& projection,
                                          rt::TrackListProjectionDeltaBatch const& batch) const
  {
    if (_rowCache.empty() ||
        std::ranges::all_of(
          batch.deltas, [](auto const& delta) { return std::holds_alternative<rt::ProjectionRemoveRange>(delta); }))
    {
      return;
    }

    auto const revision = _reads.snapshot().revision();

    if (revision == _observedRevision)
    {
      return;
    }

    auto bound = std::vector<TrackId>{};

    // The projection already has its final order. A move or filter entry may
    // have an insertion but no update.
    for (auto const& delta : batch.deltas)
    {
      if (auto const* reset = std::get_if<rt::ProjectionReset>(&delta); reset != nullptr)
      {
        if (reset->optAffectedTrackIds)
        {
          for (auto const id : *reset->optAffectedTrackIds)
          {
            markCachedRow(id, revision, bound);
          }
        }
        else
        {
          markCachedRange(projection, {.start = 0, .count = projection.size()}, revision, bound);
        }
      }
      else if (auto const* insert = std::get_if<rt::ProjectionInsertRange>(&delta); insert != nullptr)
      {
        markCachedRange(projection, insert->range, revision, bound);
      }
      else if (auto const* update = std::get_if<rt::ProjectionUpdateRange>(&delta); update != nullptr)
      {
        markCachedRange(projection, update->range, revision, bound);
      }
    }

    // A cell callback may look up another row. Mark the entire batch first,
    // and never keep an iterator into the cache across such a callback.
    for (auto const id : bound)
    {
      trackRow(id);
    }
  }
} // namespace ao::gtk
