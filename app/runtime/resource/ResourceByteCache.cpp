// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/resource/ResourceByteCache.h>

#include <ao/CoreIds.h>
#include <ao/rt/resource/ResourceBytes.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace ao::rt
{
  ResourceByteCache::ResourceByteCache(std::size_t const maximumEntries, std::size_t const maximumBytes)
    : _maximumEntries{std::max<std::size_t>(1, maximumEntries)}, _maximumBytes{std::max<std::size_t>(1, maximumBytes)}
  {
    _entries.reserve(_maximumEntries);
  }

  void ResourceByteCache::reset()
  {
    _entries.clear();
    _cachedBytes = 0;
    _useSequence = 0;
  }

  bool ResourceByteCache::store(ResourceId const resourceId, ResourceBytes bytes)
  {
    if (resourceId == kInvalidResourceId || bytes.empty())
    {
      return false;
    }

    auto const byteCount = bytes.view().size();

    if (byteCount > _maximumBytes)
    {
      return false;
    }

    if (auto it = _entries.find(resourceId); it != _entries.end())
    {
      _cachedBytes -= it->second.bytes.view().size();
      it->second.bytes = std::move(bytes);
      it->second.lastUse = ++_useSequence;
      _cachedBytes += byteCount;

      while (_cachedBytes > _maximumBytes)
      {
        evictLeastRecentlyUsed();
      }

      return true;
    }

    while (_entries.size() >= _maximumEntries || _cachedBytes > _maximumBytes - byteCount)
    {
      evictLeastRecentlyUsed();
    }

    _entries.emplace(resourceId, Entry{.bytes = std::move(bytes), .lastUse = ++_useSequence});
    _cachedBytes += byteCount;
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
      _cachedBytes -= oldest->second.bytes.view().size();
      _entries.erase(oldest);
    }
  }
} // namespace ao::rt
