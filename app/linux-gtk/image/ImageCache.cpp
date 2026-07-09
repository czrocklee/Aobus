// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ImageCache.h"

#include <ao/CoreIds.h>

#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace ao::gtk
{
  ImageCacheKey ImageCacheKey::thumbnail(ResourceId resourceId, std::int32_t physicalPixelSize) noexcept
  {
    return {.resourceId = resourceId, .physicalPixelSize = physicalPixelSize, .fullSize = false};
  }

  ImageCacheKey ImageCacheKey::full(ResourceId resourceId) noexcept
  {
    return {.resourceId = resourceId, .physicalPixelSize = 0, .fullSize = true};
  }

  std::size_t ImageCacheKeyHash::operator()(ImageCacheKey const& key) const noexcept
  {
    auto const idHash = std::hash<ResourceId>{}(key.resourceId);
    auto const sizeHash = std::hash<std::int32_t>{}(key.physicalPixelSize);
    auto const bucketHash = std::hash<bool>{}(key.fullSize);
    constexpr std::uint32_t kGoldenRatio = 0x9e3779b9U;
    constexpr std::uint32_t kHashShiftLeft = 6U;
    constexpr std::uint32_t kHashShiftRight = 2U;
    auto const sizeMixed = sizeHash + kGoldenRatio + (idHash << kHashShiftLeft) + (idHash >> kHashShiftRight);
    return idHash ^ sizeMixed ^
           (bucketHash + kGoldenRatio + (sizeHash << kHashShiftLeft) + (sizeHash >> kHashShiftRight));
  }

  ImageCache::ImageCache(std::size_t maxSize)
    : _maxSize{maxSize}
  {
  }

  ImageCache::~ImageCache() = default;

  Glib::RefPtr<Gdk::Pixbuf> ImageCache::get(ImageCacheKey key)
  {
    auto const it = _entryByKey.find(key);

    if (it == _entryByKey.end())
    {
      return {};
    }

    // Move to front (Most Recently Used)
    _entries.splice(_entries.begin(), _entries, it->second);
    return it->second->pixbufPtr;
  }

  void ImageCache::put(ImageCacheKey key, Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
  {
    if (!pixbufPtr)
    {
      return;
    }

    if (auto const it = _entryByKey.find(key); it != _entryByKey.end())
    {
      // Update existing entry and move to front
      it->second->pixbufPtr = pixbufPtr;
      _entries.splice(_entries.begin(), _entries, it->second);
      return;
    }

    // Add new entry to front
    _entries.push_front({.key = key, .pixbufPtr = pixbufPtr});
    _entryByKey[key] = _entries.begin();

    // Evict least recently used if over capacity
    if (_entries.size() > _maxSize)
    {
      auto const last = CacheEntry{_entries.back()};
      _entryByKey.erase(last.key);
      _entries.pop_back();
    }
  }

  void ImageCache::clear()
  {
    _entryByKey.clear();
    _entries.clear();
  }
} // namespace ao::gtk
