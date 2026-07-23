// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ResourceImageLoader.h"

#include "image/ImageCache.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/Log.h>
#include <ao/rt/library/LibraryTaskService.h>

#include <gdkmm/pixbuf.h>
#include <gdkmm/pixbufloader.h>
#include <glibmm/error.h>
#include <glibmm/refptr.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    constexpr std::int32_t kMaximumDecodedDimension = 8192;
    constexpr std::uint64_t kMaximumDecodedPixels = 32'000'000;
    constexpr std::size_t kDecodeInputChunkBytes = 4096;

    bool dimensionsWithinLimits(std::int32_t const width, std::int32_t const height)
    {
      if (width <= 0 || height <= 0 || width > kMaximumDecodedDimension || height > kMaximumDecodedDimension)
      {
        return false;
      }

      return static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) <= kMaximumDecodedPixels;
    }

    Glib::RefPtr<Gdk::Pixbuf> decodePixbuf(std::span<std::byte const> const bytes, ImageCacheKey const key)
    {
      auto loaderPtr = Gdk::PixbufLoader::create();
      bool sizePrepared = false;
      bool rejected = false;
      [[maybe_unused]] auto sizeConnection = loaderPtr->signal_size_prepared().connect(
        [&](std::int32_t const width, std::int32_t const height)
        {
          sizePrepared = true;

          if (!dimensionsWithinLimits(width, height))
          {
            rejected = true;
            loaderPtr->set_size(1, 1);
            return;
          }

          if (!key.fullSize)
          {
            auto const target = std::max(1, key.physicalPixelSize);
            auto targetWidth = target;
            auto targetHeight = target;

            if (width > height)
            {
              targetHeight =
                std::max(1, static_cast<std::int32_t>((static_cast<std::int64_t>(height) * target) / width));
            }
            else
            {
              targetWidth =
                std::max(1, static_cast<std::int32_t>((static_cast<std::int64_t>(width) * target) / height));
            }

            loaderPtr->set_size(targetWidth, targetHeight);
          }
        });

      std::size_t offset = 0;

      while (offset < bytes.size() && !rejected)
      {
        auto const chunkSize = std::min(kDecodeInputChunkBytes, bytes.size() - offset);
        // Gdk's C API consumes the same raw object representation as unsigned bytes.
        auto const* chunk = reinterpret_cast<guint8 const*>(bytes.data() + offset); // NOLINT
        loaderPtr->write(chunk, chunkSize);
        offset += chunkSize;
      }

      try
      {
        loaderPtr->close();
      }
      catch (...)
      {
        if (!rejected)
        {
          throw;
        }
      }

      return rejected || !sizePrepared ? Glib::RefPtr<Gdk::Pixbuf>{} : loaderPtr->get_pixbuf();
    }
  } // namespace

  ResourceImageLoader::ResourceImageLoader(rt::LibraryTaskService& tasks, ImageCache& cache, async::Runtime& runtime)
    : _tasks{tasks}, _cache{cache}, _runtime{runtime}, _scopePtr{std::make_unique<async::LifetimeScope>()}
  {
  }

  ResourceImageLoader::~ResourceImageLoader()
  {
    _scopePtr->cancelAll();
  }

  Glib::RefPtr<Gdk::Pixbuf> ResourceImageLoader::getFull(ResourceId const resourceId)
  {
    return get(ImageCacheKey::full(resourceId));
  }

  Glib::RefPtr<Gdk::Pixbuf> ResourceImageLoader::getThumbnail(ResourceId const resourceId,
                                                              std::int32_t const physicalPixelSize)
  {
    auto const requiredPixels = std::max(1, physicalPixelSize);
    auto cachedPtr = get(ImageCacheKey::thumbnail(resourceId, requiredPixels));

    if (!cachedPtr)
    {
      return {};
    }

    auto const largestDimension = std::max(cachedPtr->get_width(), cachedPtr->get_height());
    return largestDimension >= requiredPixels ? cachedPtr : Glib::RefPtr<Gdk::Pixbuf>{};
  }

  ResourceImageLoader::Request ResourceImageLoader::requestFull(ResourceId const resourceId, OnImageReady onReady)
  {
    return request(ImageCacheKey::full(resourceId), std::move(onReady));
  }

  ResourceImageLoader::Request ResourceImageLoader::requestThumbnail(ResourceId const resourceId,
                                                                     std::int32_t const physicalPixelSize,
                                                                     OnImageReady onReady)
  {
    return request(ImageCacheKey::thumbnail(resourceId, std::max(1, physicalPixelSize)), std::move(onReady));
  }

  void ResourceImageLoader::prefetchThumbnail(ResourceId const resourceId, std::int32_t const physicalPixelSize)
  {
    prefetch(ImageCacheKey::thumbnail(resourceId, std::max(1, physicalPixelSize)));
  }

  Glib::RefPtr<Gdk::Pixbuf> ResourceImageLoader::get(ImageCacheKey const key)
  {
    if (key.resourceId == kInvalidResourceId)
    {
      return {};
    }

    return _cache.get(key);
  }

  ResourceImageLoader::Request ResourceImageLoader::request(ImageCacheKey const key, OnImageReady onReady)
  {
    if (key.resourceId == kInvalidResourceId)
    {
      if (onReady)
      {
        onReady(Glib::RefPtr<Gdk::Pixbuf>{});
      }

      return {};
    }

    auto cachedPtr = key.fullSize ? getFull(key.resourceId) : getThumbnail(key.resourceId, key.physicalPixelSize);

    if (cachedPtr)
    {
      if (onReady)
      {
        onReady(cachedPtr);
      }

      return {};
    }

    auto statePtr = std::shared_ptr<RequestState>{};
    auto requestHandle = Request{};

    if (onReady)
    {
      statePtr = std::make_shared<RequestState>();
      requestHandle = Request{[statePtr] { statePtr->active.store(false, std::memory_order_relaxed); }};
    }

    if (auto const it = _inFlight.find(key); it != _inFlight.end())
    {
      if (onReady)
      {
        it->second.push_back({.statePtr = std::move(statePtr), .onReady = std::move(onReady)});
      }

      return requestHandle;
    }

    if (auto& waiters = _inFlight[key]; onReady)
    {
      waiters.push_back({.statePtr = std::move(statePtr), .onReady = std::move(onReady)});
    }

    spawnDecode(key);
    return requestHandle;
  }

  void ResourceImageLoader::prefetch(ImageCacheKey const key)
  {
    if (key.resourceId == kInvalidResourceId)
    {
      return;
    }

    auto const cachedPtr = key.fullSize ? getFull(key.resourceId) : getThumbnail(key.resourceId, key.physicalPixelSize);

    if (cachedPtr || _inFlight.contains(key))
    {
      return;
    }

    _inFlight.try_emplace(key);
    spawnDecode(key);
  }

  void ResourceImageLoader::spawnDecode(ImageCacheKey const key)
  {
    // Runtime and its task service outlive this loader. Loader-owned state is
    // touched only after the cancellation-checked callback-executor hop.
    _runtime.spawnWithLifetime(
      _scopePtr.get(),
      [loader = this, tasks = &_tasks, runtime = &_runtime, key](std::stop_token const stopToken)
      { return decode(loader, tasks, runtime, key, stopToken); });
  }

  async::Task<void> ResourceImageLoader::decode(ResourceImageLoader* const loader,
                                                rt::LibraryTaskService* const tasks,
                                                async::Runtime* const runtime,
                                                ImageCacheKey const key,
                                                std::stop_token const stopToken)
  {
    auto decodedPtr = Glib::RefPtr<Gdk::Pixbuf>{};

    try
    {
      auto bytesResult = co_await tasks->loadResourceAsync(key.resourceId, stopToken);

      if (!bytesResult)
      {
        if (bytesResult.error().code == Error::Code::ValueTooLarge)
        {
          APP_LOG_WARN("GTK cover resource {} exceeds the interactive byte limit", key.resourceId.raw());
        }
      }
      else if (*bytesResult)
      {
        auto bytes = std::move(**bytesResult);
        co_await runtime->resumeOnWorker(stopToken);

        try
        {
          decodedPtr = decodePixbuf(bytes, key);
        }
        catch (Glib::Error const&)
        {
          decodedPtr.reset();
        }
      }
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();
      runtime->reportUnhandledException(std::current_exception(), "GTK resource image decode workflow");
    }

    co_await runtime->resumeOnCallbackExecutor(stopToken);

    if (decodedPtr && !loader->get(key))
    {
      loader->_cache.put(key, decodedPtr);
    }

    auto waiters = std::vector<RequestWaiter>{};

    if (auto const it = loader->_inFlight.find(key); it != loader->_inFlight.end())
    {
      waiters = std::move(it->second);
      loader->_inFlight.erase(it);
    }

    for (auto const& waiter : waiters)
    {
      if (waiter.statePtr->active.load(std::memory_order_relaxed))
      {
        waiter.onReady(decodedPtr);
      }
    }
  }
} // namespace ao::gtk
