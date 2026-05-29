// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>

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
      Glib::RefPtr<Gdk::Pixbuf> pixbufPtr;
    };

    std::size_t _maxSize;
    std::list<CacheEntry> _entries;
    std::unordered_map<std::uint64_t, std::list<CacheEntry>::iterator> _cacheMap;
  };
} // namespace ao::gtk
