// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "image/ImageCache.h"
#include <ao/CoreIds.h>
#include <ao/async/Task.h>
#include <ao/utility/ScopedRegistration.h>

#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <unordered_map>
#include <vector>

namespace ao::rt
{
  class LibraryTaskService;
}

namespace ao::async
{
  class Runtime;
  class LifetimeScope;
}

namespace ao::gtk
{
  /**
   * Shared GTK cover-art decoder for full-size and thumbnail requests.
   *
   * Equal cache keys share one worker decode. Successful work is salvaged into
   * the cache even after every individual callback interest is cancelled.
   * Public methods and loader-owned state are confined to the GTK callback
   * executor.
   */
  class ResourceImageLoader final
  {
  public:
    using OnImageReady = std::function<void(Glib::RefPtr<Gdk::Pixbuf> const&)>;
    using Request = utility::ScopedRegistration;

    ResourceImageLoader(rt::LibraryTaskService& tasks, ImageCache& cache, async::Runtime& runtime);
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
    void prefetchThumbnail(ResourceId resourceId, std::int32_t physicalPixelSize);

  private:
    struct RequestState final
    {
      std::atomic_bool active{true};
    };

    struct RequestWaiter final
    {
      std::shared_ptr<RequestState> statePtr;
      OnImageReady onReady;
    };

    Glib::RefPtr<Gdk::Pixbuf> get(ImageCacheKey key);
    Request request(ImageCacheKey key, OnImageReady onReady);
    void prefetch(ImageCacheKey key);
    void spawnDecode(ImageCacheKey key);
    static async::Task<void> decode(ResourceImageLoader* loader,
                                    rt::LibraryTaskService* tasks,
                                    async::Runtime* runtime,
                                    ImageCacheKey key,
                                    std::stop_token stopToken);

    rt::LibraryTaskService& _tasks;
    ImageCache& _cache;
    async::Runtime& _runtime;
    std::unique_ptr<async::LifetimeScope> _scopePtr;
    std::unordered_map<ImageCacheKey, std::vector<RequestWaiter>, ImageCacheKeyHash> _inFlight;
  };
} // namespace ao::gtk
