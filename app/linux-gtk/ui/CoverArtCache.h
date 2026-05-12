// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>

#include <cstdint>
#include <list>
#include <unordered_map>

namespace ao::gtk
{
  /**
   * @brief CoverArtCache provides an LRU cache for track cover art Pixbufs.
   * Keyed by ResourceId (the ID of the image blob in the database).
   */
  class CoverArtCache final
  {
  public:
    explicit CoverArtCache(std::size_t maxSize = 50);
    ~CoverArtCache();

    /**
     * @brief Try to get a Pixbuf from the cache.
     * @param resourceId The ID of the cover art resource.
     * @return The cached Pixbuf or an empty RefPtr if not found.
     */
    Glib::RefPtr<Gdk::Pixbuf> get(std::uint64_t resourceId);

    /**
     * @brief Add a Pixbuf to the cache.
     * @param resourceId The ID of the cover art resource.
     * @param pixbuf The Pixbuf to cache.
     */
    void put(std::uint64_t resourceId, Glib::RefPtr<Gdk::Pixbuf> const& pixbuf);

    /**
     * @brief Clear all cached entries.
     */
    void clear();

  private:
    struct CacheEntry final
    {
      std::uint64_t resourceId;
      Glib::RefPtr<Gdk::Pixbuf> pixbuf;
    };

    std::size_t _maxSize;
    std::list<CacheEntry> _entries;
    std::unordered_map<std::uint64_t, std::list<CacheEntry>::iterator> _cacheMap;
  };
} // namespace ao::gtk
