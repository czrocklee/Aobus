// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ResourceImageLoader.h"

#include "image/ImageCache.h"
#include "image/ImageRenderPolicy.h"
#include <ao/CoreIds.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/resource/ResourceByteLoader.h>
#include <ao/rt/resource/ResourceBytes.h>

#include <gdkmm/pixbuf.h>
#include <gdkmm/pixbufloader.h>
#include <glibmm/error.h>
#include <glibmm/refptr.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <utility>

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
      catch (Glib::Error const&)
      {
        if (!rejected)
        {
          throw;
        }
      }

      return rejected || !sizePrepared ? Glib::RefPtr<Gdk::Pixbuf>{} : loaderPtr->get_pixbuf();
    }
  } // namespace

  ResourceImageLoader::ResourceImageLoader(rt::ResourceByteLoader& byteLoader,
                                           ImageCache& cache,
                                           async::Runtime& runtime)
    : _byteLoader{byteLoader}, _cache{cache}, _runtime{runtime}, _scopePtr{std::make_unique<async::LifetimeScope>()}
  {
  }

  ResourceImageLoader::~ResourceImageLoader()
  {
    _scopePtr->cancelAll();
    _requests.clear();
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

  ResourceImageLoader::Request ResourceImageLoader::requestHighQualityRender(Glib::RefPtr<Gdk::Pixbuf> sourcePixbufPtr,
                                                                             RenderTarget const renderedSize,
                                                                             OnImageReady onReady)
  {
    if (!sourcePixbufPtr || renderedSize.width <= 0 || renderedSize.height <= 0)
    {
      if (onReady)
      {
        onReady({});
      }

      return {};
    }

    return _runtime.spawnCancellable(
      [runtime = &_runtime, sourcePixbufPtr = std::move(sourcePixbufPtr), renderedSize, onReady = std::move(onReady)](
        std::stop_token const stopToken) mutable
      { return render(runtime, std::move(sourcePixbufPtr), renderedSize, std::move(onReady), stopToken); },
      "GTK high-quality image render workflow");
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

    auto callback = decltype(_requests)::Callback{};

    if (onReady)
    {
      callback = [onReady = std::move(onReady)](Glib::RefPtr<Gdk::Pixbuf> const& imagePtr) { onReady(imagePtr); };
    }

    return _requests.request(
      key, std::move(callback), [this, key](Requests::FlightToken token) { requestBytes(key, std::move(token)); });
  }

  void ResourceImageLoader::prefetch(ImageCacheKey const key)
  {
    if (key.resourceId == kInvalidResourceId)
    {
      return;
    }

    auto const cachedPtr = key.fullSize ? getFull(key.resourceId) : getThumbnail(key.resourceId, key.physicalPixelSize);

    if (cachedPtr)
    {
      return;
    }

    _requests.prefetch(key, [this, key](Requests::FlightToken token) { requestBytes(key, std::move(token)); });
  }

  void ResourceImageLoader::requestBytes(ImageCacheKey const key, Requests::FlightToken token)
  {
    auto dependency = _byteLoader.request(key.resourceId,
                                          [this, key, token](rt::ResourceBytes bytes) mutable
                                          { spawnDecode(key, std::move(token), std::move(bytes)); });
    _requests.retainDependency(token, std::move(dependency));
  }

  void ResourceImageLoader::spawnDecode(ImageCacheKey const key, Requests::FlightToken token, rt::ResourceBytes bytes)
  {
    // Runtime and its task service outlive this loader. Loader-owned state is
    // touched only after the cancellation-checked callback-executor hop.
    _runtime.spawnWithLifetime(
      _scopePtr.get(),
      [loader = this, runtime = &_runtime, key, token = std::move(token), bytes = std::move(bytes)](
        std::stop_token const stopToken) mutable
      { return decode(loader, runtime, key, std::move(token), std::move(bytes), stopToken); },
      "GTK resource image decode workflow");
  }

  async::Task<void> ResourceImageLoader::decode(ResourceImageLoader* const loader,
                                                async::Runtime* const runtime,
                                                ImageCacheKey const key,
                                                Requests::FlightToken token,
                                                rt::ResourceBytes bytes,
                                                std::stop_token const stopToken)
  {
    auto decodedPtr = Glib::RefPtr<Gdk::Pixbuf>{};

    co_await runtime->resumeOnWorker(stopToken);

    if (!bytes.empty())
    {
      try
      {
        decodedPtr = decodePixbuf(bytes.view(), key);
      }
      catch (Glib::Error const&)
      {
        decodedPtr.reset();
      }
    }

    co_await runtime->resumeOnCallbackExecutor(stopToken);

    if (decodedPtr && !loader->get(key))
    {
      loader->_cache.put(key, decodedPtr);
    }

    loader->_requests.complete(token, decodedPtr);
  }

  async::Task<void> ResourceImageLoader::render(async::Runtime* const runtime,
                                                Glib::RefPtr<Gdk::Pixbuf> sourcePixbufPtr,
                                                RenderTarget const renderedSize,
                                                OnImageReady onReady,
                                                std::stop_token const stopToken)
  {
    auto renderedPixbufPtr = Glib::RefPtr<Gdk::Pixbuf>{};

    co_await runtime->resumeOnWorker(stopToken);

    try
    {
      // Pixbuf pixel storage is immutable for the duration of this request.
      // GTK widgets and textures remain confined to the callback executor.
      renderedPixbufPtr =
        sourcePixbufPtr->scale_simple(renderedSize.width, renderedSize.height, Gdk::InterpType::HYPER);
    }
    catch (Glib::Error const&)
    {
      renderedPixbufPtr.reset();
    }

    co_await runtime->resumeOnCallbackExecutor(stopToken);

    if (onReady)
    {
      onReady(renderedPixbufPtr);
    }
  }
} // namespace ao::gtk
