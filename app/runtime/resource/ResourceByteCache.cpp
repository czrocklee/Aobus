// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/resource/ResourceByteCache.h>
#include <ao/rt/resource/ResourceBytes.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace ao::rt
{
  ResourceByteCache::ResourceByteCache(std::size_t const maximumEntries)
    : _maximumEntries{std::max<std::size_t>(1, maximumEntries)}
  {
    _entries.reserve(_maximumEntries);
  }

  void ResourceByteCache::reset()
  {
    _entries.clear();
    _useSequence = 0;
  }

  bool ResourceByteCache::store(ResourceId const resourceId, ResourceBytes bytes)
  {
    if (resourceId == kInvalidResourceId || bytes.empty())
    {
      return false;
    }

    if (auto it = _entries.find(resourceId); it != _entries.end())
    {
      it->second.bytes = std::move(bytes);
      it->second.lastUse = ++_useSequence;
      return true;
    }

    if (_entries.size() == _maximumEntries)
    {
      evictLeastRecentlyUsed();
    }

    _entries.emplace(resourceId, Entry{.bytes = std::move(bytes), .lastUse = ++_useSequence});
    return true;
  }

  ResourceBytes ResourceByteCache::cached(ResourceId const resourceId)
  {
    if (auto it = _entries.find(resourceId); it != _entries.end())
    {
      it->second.lastUse = ++_useSequence;
      return it->second.bytes;
    }

    return {};
  }

  void ResourceByteCache::evictLeastRecentlyUsed()
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
} // namespace ao::rt
