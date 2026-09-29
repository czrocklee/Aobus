// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "image/ImageCache.h"
#include "image/ImageRenderPolicy.h"
#include <ao/CoreIds.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/RequestCoalescer.h>
#include <ao/async/Task.h>
#include <ao/rt/resource/ResourceBytes.h>
#include <ao/utility/ScopedRegistration.h>

#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>

#include <cstdint>
#include <functional>
#include <stop_token>
#include <unordered_set>

namespace ao::rt
{
  class ResourceByteMemoryCache;
}

namespace ao::async
{
  class Runtime;
}

namespace ao::gtk
{
  /**
   * Shared GTK cover-art decoder for full-size and thumbnail requests.
   *
   * Equal cache keys share one worker decodeAsync. Successful work is salvaged into
   * the cache even after every individual callback interest is cancelled.
   * Public methods and loader-owned state are confined to the GTK callback
   * executor.
   */
  class ResourceImageLoader final
  {
  public:
    using OnImageReady = std::function<void(Glib::RefPtr<Gdk::Pixbuf> const&)>;
    using Request = utility::ScopedRegistration;

    ResourceImageLoader(rt::ResourceByteMemoryCache& byteCache, ImageCache& cache, async::Runtime& runtime);
    ~ResourceImageLoader();

    ResourceImageLoader(ResourceImageLoader const&) = delete;
    ResourceImageLoader& operator=(ResourceImageLoader const&) = delete;
    ResourceImageLoader(ResourceImageLoader&&) = delete;
    ResourceImageLoader& operator=(ResourceImageLoader&&) = delete;

    ImageCache& cache() const noexcept { return _cache; }

    Glib::RefPtr<Gdk::Pixbuf> getFull(ResourceId resourceId);
    Glib::RefPtr<Gdk::Pixbuf> getThumbnail(ResourceId resourceId, std::int32_t physicalPixelSize);

    Request requestFull(ResourceId resourceId, OnImageReady onReady);
    Request requestThumbnail(ResourceId resourceId, std::int32_t physicalPixelSize, OnImageReady onReady);
    Request requestHighQualityRender(Glib::RefPtr<Gdk::Pixbuf> sourcePixbufPtr,
                                     RenderTarget renderedSize,
                                     OnImageReady onReady);
    void prefetchThumbnail(ResourceId resourceId, std::int32_t physicalPixelSize);

  private:
    using Requests = async::RequestCoalescer<ImageCacheKey, Glib::RefPtr<Gdk::Pixbuf>, ImageCacheKeyHash>;

    Glib::RefPtr<Gdk::Pixbuf> get(ImageCacheKey key);
    Request request(ImageCacheKey key, OnImageReady onReady);
    void prefetch(ImageCacheKey key);
    void requestBytes(ImageCacheKey key, Requests::FlightToken token);
    void spawnDecode(ImageCacheKey key, Requests::FlightToken token, rt::ResourceBytes bytes);
    static async::Task<void> decodeAsync(ResourceImageLoader* loader,
                                         async::Runtime* runtime,
                                         ImageCacheKey key,
                                         Requests::FlightToken token,
                                         rt::ResourceBytes bytes,
                                         std::stop_token stopToken);
    static async::Task<void> renderAsync(async::Runtime* runtime,
                                         Glib::RefPtr<Gdk::Pixbuf> sourcePixbufPtr,
                                         RenderTarget renderedSize,
                                         OnImageReady onReady,
                                         std::stop_token stopToken);

    rt::ResourceByteMemoryCache& _byteCache;
    ImageCache& _cache;
    async::Runtime& _runtime;
    async::LifetimeScope _scope;
    Requests _requests;
    // Callback-executor-confined set of resources already logged as undecodable,
    // so repeated requests for corrupt art (for example while scrolling) do not
    // flood the log. The set grows monotonically by at most one entry per corrupt
    // resource id and is never cleared: no existing cache or library reset path
    // touches this loader (ImageCache::clear() is unused), so the bound is one id
    // per distinct corrupt resource loaded during the loader's lifetime.
    std::unordered_set<ResourceId> _decodeFailureLog;
  };
} // namespace ao::gtk
