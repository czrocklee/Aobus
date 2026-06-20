// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>
#include <ao/utility/ScopedRegistration.h>

#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ao::rt
{
  class Library;
}

namespace ao::async
{
  class Runtime;
  class LifetimeScope;
}

namespace ao::gtk
{
  class ImageCache;

  /**
   * ThumbnailLoader - shared, off-thread cover-art thumbnail decoder.
   *
   * Multiple thumbnail widgets (e.g. recycled section headers in a scrolling
   * list) share one loader so that:
   *  - Concurrent requests for the same resource are coalesced into a single
   *    decode (in-flight dedupe) — the first caller spawns the worker, later
   *    callers attach to it.
   *  - A decode that completes still populates the shared cache even if the
   *    requesting widget has moved on, so the work is never wasted (salvage).
   *  - Upcoming covers can be warmed ahead of time via prefetch().
   *
   * The loader owns an application-lifetime decode scope: decodes run to
   * completion to warm the cache regardless of any single widget's lifetime.
   * Destroying a returned Request cancels only that requester's callback. Per-widget
   * staleness (a recycled row that now shows a different track) is still the
   * caller's concern.
   *
   * All public methods must be called on the UI thread.
   */
  class ThumbnailLoader final
  {
  public:
    using OnThumbnailReady = std::function<void(Glib::RefPtr<Gdk::Pixbuf> const&)>;
    using Request = utility::ScopedRegistration;

    ThumbnailLoader(rt::Library const& reads, ImageCache& cache, async::Runtime& runtime);
    ~ThumbnailLoader();

    ThumbnailLoader(ThumbnailLoader const&) = delete;
    ThumbnailLoader& operator=(ThumbnailLoader const&) = delete;
    ThumbnailLoader(ThumbnailLoader&&) = delete;
    ThumbnailLoader& operator=(ThumbnailLoader&&) = delete;

    /// The shared thumbnail cache backing this loader.
    ImageCache& cache() const noexcept { return _cache; }

    /// Synchronous cache lookup; empty RefPtr on miss or undersized cached entry.
    Glib::RefPtr<Gdk::Pixbuf> get(ResourceId id, std::int32_t physicalPixelSize);

    /**
     * Request a thumbnail for @p id decoded at roughly @p physicalPixelSize. On a
     * cache hit @p onReady is invoked synchronously; otherwise the decode runs off
     * the UI thread and @p onReady is invoked on the UI thread when it completes
     * (with an empty RefPtr if the decode failed). Concurrent requests for the same
     * id and physical pixel size are coalesced into one decode. Keep the returned
     * request alive to receive the asynchronous callback; destroying it cancels that
     * callback without cancelling the shared decode.
     */
    Request request(ResourceId id, std::int32_t physicalPixelSize, OnThumbnailReady onReady);

    /**
     * Warm the cache for @p id without a callback. @p physicalPixelSize is in
     * device pixels, not logical CSS pixels. No-op if invalid, already cached at
     * sufficient size, or already in flight at the same size.
     */
    void prefetch(ResourceId id, std::int32_t physicalPixelSize);

  private:
    struct RequestKey final
    {
      ResourceId id;
      std::int32_t physicalPixelSize{};

      bool operator==(RequestKey const&) const = default;
    };

    struct RequestKeyHash final
    {
      std::size_t operator()(RequestKey const& key) const noexcept;
    };

    struct RequestState final
    {
      std::atomic_bool active{true};
    };

    struct RequestWaiter final
    {
      std::shared_ptr<RequestState> statePtr;
      OnThumbnailReady onReady;
    };

    void spawnDecode(RequestKey key);

    rt::Library const& _reads;
    ImageCache& _cache;
    async::Runtime& _runtime;
    std::unique_ptr<async::LifetimeScope> _scopePtr;

    // Resources currently being decoded, mapped to the callbacks waiting on them.
    std::unordered_map<RequestKey, std::vector<RequestWaiter>, RequestKeyHash> _inFlight;
  };
} // namespace ao::gtk
