// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "inspector/CoverArtCache.h"

#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>

#include <cstddef>
#include <cstdint>

namespace ao::gtk
{
  CoverArtCache::CoverArtCache(std::size_t maxSize)
    : _maxSize{maxSize}
  {
  }

  CoverArtCache::~CoverArtCache() = default;

  Glib::RefPtr<Gdk::Pixbuf> CoverArtCache::get(std::uint64_t resourceId)
  {
    auto const it = _cacheMap.find(resourceId);

    if (it == _cacheMap.end())
    {
      return {};
    }

    // Move to front (Most Recently Used)
    _entries.splice(_entries.begin(), _entries, it->second);
    return it->second->pixbuf;
  }

  void CoverArtCache::put(std::uint64_t resourceId, Glib::RefPtr<Gdk::Pixbuf> const& pixbuf)
  {
    if (!pixbuf)
    {
      return;
    }

    auto const it = _cacheMap.find(resourceId);

    if (it != _cacheMap.end())
    {
      // Update existing entry and move to front
      it->second->pixbuf = pixbuf;
      _entries.splice(_entries.begin(), _entries, it->second);
      return;
    }

    // Add new entry to front
    _entries.push_front({.resourceId = resourceId, .pixbuf = pixbuf});
    _cacheMap[resourceId] = _entries.begin();

    // Evict least recently used if over capacity
    if (_entries.size() > _maxSize)
    {
      auto const last = CacheEntry{_entries.back()};
      _cacheMap.erase(last.resourceId);
      _entries.pop_back();
    }
  }

  void CoverArtCache::clear()
  {
    _cacheMap.clear();
    _entries.clear();
  }
} // namespace ao::gtk
