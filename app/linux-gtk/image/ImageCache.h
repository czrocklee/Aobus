// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>

#include <cstddef>
#include <functional>
#include <list>

namespace ao::gtk
{
  /**
   * @brief ImageCache provides an LRU cache for track cover art Pixbufs.
   * Keyed by ResourceId (the ID of the image blob in the database).
   */
  class ImageCache final
  {
  public:
    static constexpr std::size_t kDefaultMaxSize = 50;

    explicit ImageCache(std::size_t maxSize = kDefaultMaxSize);
    ~ImageCache();

    // Not copyable or movable
    ImageCache(ImageCache const&) = delete;
    ImageCache& operator=(ImageCache const&) = delete;
    ImageCache(ImageCache&&) = delete;
    ImageCache& operator=(ImageCache&&) = delete;

    /**
     * @brief Try to get a Pixbuf from the cache.
     * @param resourceId The ID of the cover art resource.
     * @return The cached Pixbuf or an empty RefPtr if not found.
     */
    Glib::RefPtr<Gdk::Pixbuf> get(ResourceId resourceId);

    /**
     * @brief Add a Pixbuf to the cache.
     * @param resourceId The ID of the cover art resource.
     * @param pixbuf The Pixbuf to cache.
     */
    void put(ResourceId resourceId, Glib::RefPtr<Gdk::Pixbuf> const& pixbuf);

    /**
     * @brief Clear all cached entries.
     */
    void clear();

  private:
    struct CacheEntry final
    {
      ResourceId resourceId;
      Glib::RefPtr<Gdk::Pixbuf> pixbufPtr;
    };

    std::size_t _maxSize;
    std::list<CacheEntry> _entries;
    boost::unordered_flat_map<ResourceId, std::list<CacheEntry>::iterator, std::hash<ResourceId>> _cacheMap;
  };
} // namespace ao::gtk
