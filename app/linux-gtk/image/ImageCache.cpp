// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ImageCache.h"

#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>

#include <cstddef>
#include <cstdint>

namespace ao::gtk
{
  ImageCache::ImageCache(std::size_t maxSize)
    : _maxSize{maxSize}
  {
  }

  ImageCache::~ImageCache() = default;

  Glib::RefPtr<Gdk::Pixbuf> ImageCache::get(std::uint64_t resourceId)
  {
    auto const it = _cacheMap.find(resourceId);

    if (it == _cacheMap.end())
    {
      return {};
    }

    // Move to front (Most Recently Used)
    _entries.splice(_entries.begin(), _entries, it->second);
    return it->second->pixbufPtr;
  }

  void ImageCache::put(std::uint64_t resourceId, Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
  {
    if (!pixbufPtr)
    {
      return;
    }

    if (auto const it = _cacheMap.find(resourceId); it != _cacheMap.end())
    {
      // Update existing entry and move to front
      it->second->pixbufPtr = pixbufPtr;
      _entries.splice(_entries.begin(), _entries, it->second);
      return;
    }

    // Add new entry to front
    _entries.push_front({.resourceId = resourceId, .pixbufPtr = pixbufPtr});
    _cacheMap[resourceId] = _entries.begin();

    // Evict least recently used if over capacity
    if (_entries.size() > _maxSize)
    {
      auto const last = CacheEntry{_entries.back()};
      _cacheMap.erase(last.resourceId);
      _entries.pop_back();
    }
  }

  void ImageCache::clear()
  {
    _cacheMap.clear();
    _entries.clear();
  }
} // namespace ao::gtk
