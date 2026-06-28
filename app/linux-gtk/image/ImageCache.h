// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>

#include <cstddef>
#include <cstdint>
#include <list>

namespace ao::gtk
{
  /**
   * @brief ImageCache provides an LRU cache for track cover art Pixbufs.
   * Keyed by resource plus usage/size so thumbnails and full-size images cannot collide.
   */
  struct ImageCacheKey final
  {
    ResourceId resourceId{kInvalidResourceId};
    std::int32_t physicalPixelSize{};
    bool fullSize = false;

    static ImageCacheKey thumbnail(ResourceId resourceId, std::int32_t physicalPixelSize) noexcept;
    static ImageCacheKey full(ResourceId resourceId) noexcept;

    bool operator==(ImageCacheKey const&) const = default;
  };

  struct ImageCacheKeyHash final
  {
    std::size_t operator()(ImageCacheKey const& key) const noexcept;
  };

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
     * @param key The resource and usage/size bucket.
     * @return The cached Pixbuf or an empty RefPtr if not found.
     */
    Glib::RefPtr<Gdk::Pixbuf> get(ImageCacheKey key);

    /**
     * @brief Add a Pixbuf to the cache.
     * @param key The resource and usage/size bucket.
     * @param pixbuf The Pixbuf to cache.
     */
    void put(ImageCacheKey key, Glib::RefPtr<Gdk::Pixbuf> const& pixbuf);

    /**
     * @brief Clear all cached entries.
     */
    void clear();

  private:
    struct CacheEntry final
    {
      ImageCacheKey key;
      Glib::RefPtr<Gdk::Pixbuf> pixbufPtr;
    };

    std::size_t _maxSize;
    std::list<CacheEntry> _entries;
    boost::unordered_flat_map<ImageCacheKey, std::list<CacheEntry>::iterator, ImageCacheKeyHash> _cacheMap;
  };
} // namespace ao::gtk
