// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/uimodel/library/track/CoverArtRequestModel.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  CoverArtRequestModel::CoverArtRequestModel(std::size_t const maximumEntries)
    : _maximumEntries{std::max<std::size_t>(1, maximumEntries)}
  {
    _entries.reserve(_maximumEntries);
  }

  CoverArtRequestToken CoverArtRequestModel::select(ResourceId const resourceId)
  {
    _selectedResourceId = resourceId;
    ++_generation;
    return {.resourceId = resourceId, .generation = _generation};
  }

  void CoverArtRequestModel::clearSelection()
  {
    _selectedResourceId = kInvalidResourceId;
    ++_generation;
  }

  void CoverArtRequestModel::reset()
  {
    clearSelection();
    _entries.clear();
    _useSequence = 0;
  }

  bool CoverArtRequestModel::accepts(CoverArtRequestToken const token) const noexcept
  {
    return token.resourceId != kInvalidResourceId && token.resourceId == _selectedResourceId &&
           token.generation == _generation;
  }

  bool CoverArtRequestModel::store(CoverArtRequestToken const token, std::vector<std::byte> bytes)
  {
    if (!accepts(token))
    {
      return false;
    }

    if (auto it = _entries.find(token.resourceId); it != _entries.end())
    {
      it->second.bytes = std::move(bytes);
      it->second.lastUse = ++_useSequence;
      return true;
    }

    if (_entries.size() == _maximumEntries)
    {
      evictLeastRecentlyUsed();
    }

    _entries.emplace(token.resourceId, Entry{.bytes = std::move(bytes), .lastUse = ++_useSequence});
    return true;
  }

  std::span<std::byte const> CoverArtRequestModel::cached(ResourceId const resourceId)
  {
    if (auto it = _entries.find(resourceId); it != _entries.end())
    {
      it->second.lastUse = ++_useSequence;
      return it->second.bytes;
    }

    return {};
  }

  void CoverArtRequestModel::evictLeastRecentlyUsed()
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
