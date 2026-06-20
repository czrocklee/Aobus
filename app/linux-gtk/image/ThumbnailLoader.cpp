// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ThumbnailLoader.h"

#include "image/ImageCache.h"
#include <ao/Type.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>

#include <gdkmm/pixbuf.h>
#include <giomm/memoryinputstream.h>
#include <glibmm/error.h>
#include <glibmm/refptr.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ao::gtk
{
  ThumbnailLoader::ThumbnailLoader(rt::Library const& reads, ImageCache& cache, async::Runtime& runtime)
    : _reads{reads}, _cache{cache}, _runtime{runtime}, _scopePtr{std::make_unique<async::LifetimeScope>()}
  {
  }

  ThumbnailLoader::~ThumbnailLoader() = default;

  std::size_t ThumbnailLoader::RequestKeyHash::operator()(RequestKey const& key) const noexcept
  {
    auto const idHash = std::hash<ResourceId>{}(key.id);
    auto const sizeHash = std::hash<std::int32_t>{}(key.physicalPixelSize);
    constexpr std::uint32_t kGoldenRatio = 0x9e3779b9U;
    constexpr std::uint32_t kHashShiftLeft = 6U;
    constexpr std::uint32_t kHashShiftRight = 2U;
    return idHash ^ (sizeHash + kGoldenRatio + (idHash << kHashShiftLeft) + (idHash >> kHashShiftRight));
  }

  Glib::RefPtr<Gdk::Pixbuf> ThumbnailLoader::get(ResourceId id, std::int32_t physicalPixelSize)
  {
    if (id == kInvalidResourceId)
    {
      return {};
    }

    auto cachedPtr = _cache.get(id);

    if (!cachedPtr)
    {
      return {};
    }

    auto const requiredPixels = std::max(1, physicalPixelSize);
    auto const largestDimension = std::max(cachedPtr->get_width(), cachedPtr->get_height());
    return largestDimension >= requiredPixels ? cachedPtr : Glib::RefPtr<Gdk::Pixbuf>{};
  }

  ThumbnailLoader::Request ThumbnailLoader::request(ResourceId id,
                                                    std::int32_t physicalPixelSize,
                                                    OnThumbnailReady onReady)
  {
    if (id == kInvalidResourceId)
    {
      if (onReady)
      {
        onReady(Glib::RefPtr<Gdk::Pixbuf>{});
      }

      return {};
    }

    physicalPixelSize = std::max(1, physicalPixelSize);

    if (auto cachedPtr = get(id, physicalPixelSize); cachedPtr)
    {
      if (onReady)
      {
        onReady(cachedPtr);
      }

      return {};
    }

    auto const key = RequestKey{.id = id, .physicalPixelSize = physicalPixelSize};

    auto statePtr = std::shared_ptr<RequestState>{};
    auto request = Request{};

    if (onReady)
    {
      statePtr = std::make_shared<RequestState>();
      request = Request{[statePtr] { statePtr->active.store(false, std::memory_order_relaxed); }};
    }

    // Coalesce onto an existing decode if one is already running for this id.
    if (auto const it = _inFlight.find(key); it != _inFlight.end())
    {
      if (onReady)
      {
        it->second.push_back({std::move(statePtr), std::move(onReady)});
      }

      return request;
    }

    if (auto& waiters = _inFlight[key]; onReady)
    {
      waiters.push_back({std::move(statePtr), std::move(onReady)});
    }

    spawnDecode(key);
    return request;
  }

  void ThumbnailLoader::prefetch(ResourceId id, std::int32_t physicalPixelSize)
  {
    if (id == kInvalidResourceId)
    {
      return;
    }

    physicalPixelSize = std::max(1, physicalPixelSize);

    if (get(id, physicalPixelSize))
    {
      return;
    }

    auto const key = RequestKey{.id = id, .physicalPixelSize = physicalPixelSize};

    if (_inFlight.contains(key))
    {
      return;
    }

    _inFlight.try_emplace(key);
    spawnDecode(key);
  }

  void ThumbnailLoader::spawnDecode(RequestKey key)
  {
    // State the coroutine needs is passed by value into the frame; the loader
    // destruction cancels its decode scope, and the post-dispatch cancellation
    // check prevents this loader from being touched after destruction.
    _runtime.spawnWithLifetime(_scopePtr.get(),
                               [](ThumbnailLoader* self,
                                  async::Runtime* runtime,
                                  rt::Library const* reads,
                                  RequestKey requestKey) -> async::Task<void>
                               {
                                 auto decodedPtr = Glib::RefPtr<Gdk::Pixbuf>{};

                                 co_await runtime->resumeOnWorker();

                                 try
                                 {
                                   auto optData = std::optional<std::vector<std::byte>>{};

                                   {
                                     auto scope = reads->reader();
                                     optData = scope.loadResource(requestKey.id);
                                   }

                                   if (optData)
                                   {
                                     auto const memStreamPtr = Gio::MemoryInputStream::create();
                                     memStreamPtr->add_data(optData->data(), std::ssize(*optData), nullptr);
                                     decodedPtr = Gdk::Pixbuf::create_from_stream_at_scale(
                                       memStreamPtr, requestKey.physicalPixelSize, requestKey.physicalPixelSize, true);
                                   }
                                 }
                                 catch (...)
                                 {
                                   decodedPtr.reset();
                                 }

                                 // Throws operation_aborted (unwinding the frame) if the loader's scope was
                                 // cancelled, so `self` is safe to touch past this point.
                                 co_await runtime->resumeOnCallbackExecutor();

                                 // Salvage: a successful decode populates the shared cache even if every
                                 // requester has since moved on.
                                 if (decodedPtr && !self->get(requestKey.id, requestKey.physicalPixelSize))
                                 {
                                   self->_cache.put(requestKey.id, decodedPtr);
                                 }

                                 auto waiters = std::vector<RequestWaiter>{};

                                 if (auto const it = self->_inFlight.find(requestKey); it != self->_inFlight.end())
                                 {
                                   waiters = std::move(it->second);
                                   self->_inFlight.erase(it);
                                 }

                                 for (auto const& waiter : waiters)
                                 {
                                   if (waiter.statePtr->active.load(std::memory_order_relaxed))
                                   {
                                     waiter.onReady(decodedPtr);
                                   }
                                 }
                               }(this, &_runtime, &_reads, key));
  }
} // namespace ao::gtk
