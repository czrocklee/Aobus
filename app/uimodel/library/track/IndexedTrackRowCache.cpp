// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackRow.h>
#include <ao/uimodel/library/track/IndexedTrackRowCache.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace ao::uimodel
{
  IndexedTrackRowCache::IndexedTrackRowCache(std::size_t const maximumEntries)
    : _maximumEntries{std::max<std::size_t>(1, maximumEntries)}
  {
    _entries.reserve(_maximumEntries);
  }

  void IndexedTrackRowCache::reset(std::size_t const sourceSize, Loader loader)
  {
    _sourceSize = sourceSize;
    _useSequence = 0;
    _loader = std::move(loader);
    _entries.clear();
  }

  rt::TrackRow const* IndexedTrackRowCache::rowAt(std::size_t const index)
  {
    if (index >= _sourceSize || !_loader)
    {
      return nullptr;
    }

    if (auto it = _entries.find(index); it != _entries.end())
    {
      it->second.lastUse = ++_useSequence;
      return &it->second.row;
    }

    auto optRow = _loader(index);

    if (!optRow)
    {
      return nullptr;
    }

    if (_entries.size() == _maximumEntries)
    {
      evictLeastRecentlyUsed();
    }

    auto [it, inserted] = _entries.emplace(index, Entry{.row = std::move(*optRow), .lastUse = ++_useSequence});
    return inserted ? &it->second.row : nullptr;
  }

  void IndexedTrackRowCache::evictLeastRecentlyUsed()
  {
    auto oldest = _entries.end();
    auto oldestUse = std::numeric_limits<std::uint64_t>::max();

    for (auto it = _entries.begin(); it != _entries.end(); ++it)
    {
      if (it->second.lastUse < oldestUse)
      {
        oldest = it;
        oldestUse = it->second.lastUse;
      }
    }

    if (oldest != _entries.end())
    {
      _entries.erase(oldest);
    }
  }
} // namespace ao::uimodel
