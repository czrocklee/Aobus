// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ResourceImageLoader.h"

#include "image/ImageCache.h"
#include "image/ImageRenderPolicy.h"
#include "platform/MprisArtUrlCache.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/image/ImageTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/resource/ResourceByteLoader.h>

#include <catch2/catch_test_macros.hpp>
#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    async::Task<Result<std::optional<std::vector<std::byte>>>> loadEmptyAfterOneFailure(
      std::shared_ptr<std::atomic_bool> failNextPtr,
      rt::test::AsyncTestState<std::size_t> loadCount,
      ResourceId /*resourceId*/,
      std::stop_token /*stopToken*/)
    {
      loadCount.increment();

      if (failNextPtr->exchange(false))
      {
        co_return makeError(Error::Code::IoError, "injected resource load failure");
      }

      co_return std::optional<std::vector<std::byte>>{};
    }

    async::Task<Result<std::optional<std::vector<std::byte>>>> cancelResourceLoad(
      rt::test::AsyncTestState<std::size_t> loadCount,
      ResourceId /*resourceId*/,
      std::stop_token /*stopToken*/)
    {
      loadCount.increment();
      async::throwOperationCancelled();
      co_return std::optional<std::vector<std::byte>>{};
    }

    async::Task<Result<std::optional<std::vector<std::byte>>>> loadResourceAfterRelease(
      rt::test::AsyncTestState<std::size_t> loadCount,
      rt::test::AsyncBarrier* release,
      std::vector<std::byte> bytes,
      ResourceId /*resourceId*/,
      std::stop_token const stopToken)
    {
      loadCount.increment();
      release->wait();
      async::throwIfStopRequested(stopToken);
      co_return std::optional{std::move(bytes)};
    }
  } // namespace

  TEST_CASE("ResourceImageLoader - resolves image sources into pixbuf results",
            "[gtk][unit][resource-image][concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto const validBytes = encodePng(makePixbuf(256));
    constexpr auto kBadBytes = std::array{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    auto const oversizedDimensionBytes = encodePng(makePixbuf(8193, 1));
    auto validResourceId = kInvalidResourceId;
    auto malformedResourceId = kInvalidResourceId;
    auto oversizedDimensionResourceId = kInvalidResourceId;
    auto fixture =
      GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                        {
                          validResourceId = writeRawResource(musicLibrary, validBytes);
                          malformedResourceId = writeRawResource(musicLibrary, std::span<std::byte const>{kBadBytes});
                          oversizedDimensionResourceId = writeRawResource(musicLibrary, oversizedDimensionBytes);
                        }};

    for (auto const& bytes : {std::span<std::byte const>{validBytes},
                              std::span<std::byte const>{kBadBytes},
                              std::span<std::byte const>{oversizedDimensionBytes}})
    {
      installCoverCacheEntry(fixture.cacheDirectory(), bytes);
    }

    auto& runtime = fixture.runtime();
    auto cache = ImageCache{200};
    auto byteLoader = rt::ResourceByteLoader{runtime};
    auto loader = ResourceImageLoader{byteLoader, cache, runtime.async()};

    constexpr std::int32_t kPixelSize = 48;

    SECTION("full-size request decodes off-thread under a distinct cache key")
    {
      auto receivedPtr = Glib::RefPtr<Gdk::Pixbuf>{};
      auto request = loader.requestFull(
        validResourceId, [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr) { receivedPtr = pixbufPtr; });
      REQUIRE(request);

      REQUIRE(pumpGtkEventsUntil([&] { return static_cast<bool>(receivedPtr); }));
      CHECK(receivedPtr->get_width() == 256);
      CHECK(loader.getFull(validResourceId).get() == receivedPtr.get());
      CHECK_FALSE(loader.getThumbnail(validResourceId, kPixelSize));
    }

    SECTION("high-quality render publishes the worker result on the GTK executor")
    {
      auto const ownerThread = std::this_thread::get_id();
      auto sourcePixbufPtr = makePixbuf(512, 384);
      sourcePixbufPtr->fill(0x4f82baffU);
      auto renderedPixbufPtr = Glib::RefPtr<Gdk::Pixbuf>{};
      auto callbackThread = std::thread::id{};
      auto request = loader.requestHighQualityRender(sourcePixbufPtr,
                                                     RenderTarget{.width = 96, .height = 72},
                                                     [&](Glib::RefPtr<Gdk::Pixbuf> const& resultPtr)
                                                     {
                                                       callbackThread = std::this_thread::get_id();
                                                       renderedPixbufPtr = resultPtr;
                                                     });
      REQUIRE(request);

      REQUIRE(pumpGtkEventsUntil([&] { return static_cast<bool>(renderedPixbufPtr); }));
      CHECK(callbackThread == ownerThread);
      CHECK(renderedPixbufPtr->get_width() == 96);
      CHECK(renderedPixbufPtr->get_height() == 72);
      // Rendered frames are widget-owned and intentionally do not expand the
      // shared decoded-resource cache.
      CHECK_FALSE(loader.getThumbnail(validResourceId, 96));
    }

    SECTION("request decodes off-thread, populates the cache, and invokes the callback")
    {
      auto const resourceId = validResourceId;

      auto receivedPtr = Glib::RefPtr<Gdk::Pixbuf>{};
      std::int32_t callbackCount = 0;
      auto request = loader.requestThumbnail(resourceId,
                                             kPixelSize,
                                             [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
                                             {
                                               receivedPtr = pixbufPtr;
                                               ++callbackCount;
                                             });
      REQUIRE(request);

      REQUIRE(pumpGtkEventsUntil([&] { return callbackCount > 0; }));
      CHECK(callbackCount == 1);
      REQUIRE(receivedPtr);
      // Decode-at-scale bounds the result below the 256px source.
      CHECK(receivedPtr->get_width() <= kPixelSize);
      CHECK(receivedPtr->get_width() < 256);

      // The shared cache now holds the same decoded pixbuf.
      auto const cachedPtr = loader.getThumbnail(resourceId, kPixelSize);
      REQUIRE(cachedPtr);
      CHECK(cachedPtr.get() == receivedPtr.get());
    }

    SECTION("concurrent requests for the same id coalesce into a single decode")
    {
      auto const resourceId = validResourceId;

      auto firstPtr = Glib::RefPtr<Gdk::Pixbuf>{};
      auto secondPtr = Glib::RefPtr<Gdk::Pixbuf>{};
      bool firstDone = false;
      bool secondDone = false;

      auto firstRequest = loader.requestThumbnail(resourceId,
                                                  kPixelSize,
                                                  [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
                                                  {
                                                    firstPtr = pixbufPtr;
                                                    firstDone = true;
                                                  });
      auto secondRequest = loader.requestThumbnail(resourceId,
                                                   kPixelSize,
                                                   [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
                                                   {
                                                     secondPtr = pixbufPtr;
                                                     secondDone = true;
                                                   });
      REQUIRE(firstRequest);
      REQUIRE(secondRequest);

      REQUIRE(pumpGtkEventsUntil([&] { return firstDone && secondDone; }));
      CHECK(firstPtr);
      CHECK(secondPtr);
      // Both callbacks receive the very same decoded object: only one decode ran.
      CHECK(firstPtr.get() == secondPtr.get());
    }

    SECTION("waiters for a coalesced decode are invoked in request order")
    {
      auto const resourceId = validResourceId;
      auto callbackOrder = std::vector<int>{};
      auto requests = std::vector<ResourceImageLoader::Request>{};

      for (std::int32_t index = 0; index < 4; ++index)
      {
        requests.push_back(loader.requestThumbnail(
          resourceId, kPixelSize, [&, index](Glib::RefPtr<Gdk::Pixbuf> const&) { callbackOrder.push_back(index); }));
      }

      REQUIRE(pumpGtkEventsUntil([&] { return callbackOrder.size() == 4; }));
      CHECK(callbackOrder == std::vector<int>{0, 1, 2, 3});
    }

    SECTION("larger requests are not satisfied by smaller in-flight decodes")
    {
      auto const resourceId = validResourceId;

      auto smallPtr = Glib::RefPtr<Gdk::Pixbuf>{};
      auto largePtr = Glib::RefPtr<Gdk::Pixbuf>{};

      auto smallRequest = loader.requestThumbnail(
        resourceId, 48, [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr) { smallPtr = pixbufPtr; });
      auto largeRequest = loader.requestThumbnail(
        resourceId, 96, [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr) { largePtr = pixbufPtr; });
      REQUIRE(smallRequest);
      REQUIRE(largeRequest);

      REQUIRE(pumpGtkEventsUntil([&] { return smallPtr && largePtr; }));
      CHECK(smallPtr.get() != largePtr.get());
      CHECK(std::max(smallPtr->get_width(), smallPtr->get_height()) <= 48);
      CHECK(std::max(largePtr->get_width(), largePtr->get_height()) >= 96);
      CHECK(loader.getThumbnail(resourceId, 96));
    }

    SECTION("prefetch warms the cache without a callback")
    {
      auto const resourceId = validResourceId;

      loader.prefetchThumbnail(resourceId, kPixelSize);

      REQUIRE(pumpGtkEventsUntil([&] { return static_cast<bool>(loader.getThumbnail(resourceId, kPixelSize)); }));
      CHECK(loader.getThumbnail(resourceId, kPixelSize));
    }

    SECTION("prefetch is a no-op for invalid, cached, and already in-flight requests")
    {
      auto const resourceId = validResourceId;

      loader.prefetchThumbnail(kInvalidResourceId, kPixelSize);
      CHECK_FALSE(loader.getThumbnail(kInvalidResourceId, kPixelSize));

      std::int32_t callbackCount = 0;
      auto request =
        loader.requestThumbnail(resourceId, kPixelSize, [&](Glib::RefPtr<Gdk::Pixbuf> const&) { ++callbackCount; });
      REQUIRE(request);
      loader.prefetchThumbnail(resourceId, kPixelSize);

      REQUIRE(pumpGtkEventsUntil([&] { return callbackCount == 1; }));
      auto const firstCachedPtr = loader.getThumbnail(resourceId, kPixelSize);
      REQUIRE(firstCachedPtr);

      loader.prefetchThumbnail(resourceId, kPixelSize);
      CHECK(loader.getThumbnail(resourceId, kPixelSize).get() == firstCachedPtr.get());
    }

    SECTION("a cache hit invokes the callback synchronously")
    {
      auto const resourceId = ResourceId{4242};
      cache.put(ImageCacheKey::thumbnail(resourceId, kPixelSize), makePixbuf(kPixelSize));

      bool invokedSynchronously = false;
      auto request = loader.requestThumbnail(resourceId,
                                             kPixelSize,
                                             [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
                                             { invokedSynchronously = static_cast<bool>(pixbufPtr); });

      // No pumping: the hit path must run inline.
      CHECK(invokedSynchronously);
      CHECK_FALSE(request);
    }

    SECTION("request accepts an empty callback and still warms the cache")
    {
      auto const resourceId = validResourceId;

      auto request = loader.requestThumbnail(resourceId, kPixelSize, ResourceImageLoader::OnImageReady{});
      CHECK_FALSE(request);

      REQUIRE(pumpGtkEventsUntil([&] { return static_cast<bool>(loader.getThumbnail(resourceId, kPixelSize)); }));
      CHECK(loader.getThumbnail(resourceId, kPixelSize));
    }

    SECTION("an invalid id reports an empty result and caches nothing")
    {
      bool called = false;
      bool wasEmpty = false;
      auto request = loader.requestThumbnail(kInvalidResourceId,
                                             kPixelSize,
                                             [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
                                             {
                                               called = true;
                                               wasEmpty = !pixbufPtr;
                                             });

      CHECK(called);
      CHECK(wasEmpty);
      CHECK_FALSE(request);
      CHECK_FALSE(loader.getThumbnail(kInvalidResourceId, kPixelSize));
    }

    SECTION("a missing resource id reports an empty result and clears the in-flight entry")
    {
      std::int32_t callbackCount = 0;
      bool wasEmpty = false;
      auto const missingId = ResourceId{987654};

      auto request = loader.requestThumbnail(missingId,
                                             kPixelSize,
                                             [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
                                             {
                                               ++callbackCount;
                                               wasEmpty = !pixbufPtr;
                                             });
      REQUIRE(request);

      REQUIRE(pumpGtkEventsUntil([&] { return callbackCount == 1; }));
      CHECK(wasEmpty);
      CHECK_FALSE(loader.getThumbnail(missingId, kPixelSize));

      auto retryRequest =
        loader.requestThumbnail(missingId, kPixelSize, [&](Glib::RefPtr<Gdk::Pixbuf> const&) { ++callbackCount; });
      REQUIRE(retryRequest);
      REQUIRE(pumpGtkEventsUntil([&] { return callbackCount == 2; }));
    }

    SECTION("malformed image bytes report an empty result and are not cached")
    {
      auto const resourceId = malformedResourceId;
      std::int32_t callbackCount = 0;
      bool wasEmpty = false;

      auto request = loader.requestThumbnail(resourceId,
                                             kPixelSize,
                                             [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
                                             {
                                               ++callbackCount;
                                               wasEmpty = !pixbufPtr;
                                             });
      REQUIRE(request);

      REQUIRE(pumpGtkEventsUntil([&] { return callbackCount == 1; }));
      CHECK(wasEmpty);
      CHECK_FALSE(loader.getThumbnail(resourceId, kPixelSize));
    }

    SECTION("source dimensions above the interactive limit are rejected before full decode")
    {
      bool completed = false;
      auto receivedPtr = Glib::RefPtr<Gdk::Pixbuf>{};
      auto request = loader.requestFull(oversizedDimensionResourceId,
                                        [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
                                        {
                                          receivedPtr = pixbufPtr;
                                          completed = true;
                                        });
      REQUIRE(request);

      REQUIRE(pumpGtkEventsUntil([&] { return completed; }));
      CHECK_FALSE(receivedPtr);
      CHECK_FALSE(loader.getFull(oversizedDimensionResourceId));
    }

    SECTION("destroying a request cancels its callback without cancelling the shared decode")
    {
      auto const resourceId = validResourceId;
      std::int32_t callbackCount = 0;

      auto request =
        loader.requestThumbnail(resourceId, kPixelSize, [&](Glib::RefPtr<Gdk::Pixbuf> const&) { ++callbackCount; });
      REQUIRE(request);

      request.reset();

      REQUIRE(pumpGtkEventsUntil([&] { return static_cast<bool>(loader.getThumbnail(resourceId, kPixelSize)); }));
      CHECK(callbackCount == 0);
    }

    SECTION("destroying the loader cancels pending callbacks")
    {
      auto const resourceId = validResourceId;
      std::int32_t callbackCount = 0;
      auto request = ResourceImageLoader::Request{};

      {
        auto scopedLoader = ResourceImageLoader{byteLoader, cache, runtime.async()};
        request = scopedLoader.requestThumbnail(
          resourceId, kPixelSize, [&](Glib::RefPtr<Gdk::Pixbuf> const&) { ++callbackCount; });
        REQUIRE(request);
      }

      CHECK(callbackCount == 0);
      request.reset();

      auto replacementLoader = ResourceImageLoader{byteLoader, cache, runtime.async()};
      std::int32_t replacementCallbackCount = 0;
      auto replacementRequest = replacementLoader.requestThumbnail(
        resourceId, kPixelSize, [&](Glib::RefPtr<Gdk::Pixbuf> const&) { ++replacementCallbackCount; });
      REQUIRE(replacementRequest);
      REQUIRE(pumpGtkEventsUntil([&] { return replacementCallbackCount == 1; }));
      CHECK(callbackCount == 0);
    }
  }

  TEST_CASE("ResourceImageLoader - failed resource loads terminate their request flight",
            "[gtk][unit][resource-image][concurrency]")
  {
    auto executor = rt::test::QueuedExecutor{};
    auto runtime = async::Runtime{executor, 1};
    auto cache = ImageCache{200};
    constexpr auto kMissingResourceId = ResourceId{987655};
    constexpr std::int32_t kPixelSize = 48;

    SECTION("a Result failure completes empty and permits retry")
    {
      auto callbackCount = rt::test::AsyncTestState<std::size_t>::create(0);
      auto loadCount = rt::test::AsyncTestState<std::size_t>::create(0);
      auto receivedImage = rt::test::AsyncTestState<bool>::create(true);
      auto failNextPtr = std::make_shared<std::atomic_bool>(true);
      auto byteLoader =
        rt::ResourceByteLoader{runtime, std::bind_front(loadEmptyAfterOneFailure, failNextPtr, loadCount)};
      auto loader = ResourceImageLoader{byteLoader, cache, runtime};

      auto request = loader.requestThumbnail(kMissingResourceId,
                                             kPixelSize,
                                             [callbackCount, receivedImage](Glib::RefPtr<Gdk::Pixbuf> const& imagePtr)
                                             {
                                               receivedImage.set(static_cast<bool>(imagePtr));
                                               callbackCount.increment();
                                             });
      REQUIRE(request);
      REQUIRE(executor.drainUntil([&] { return callbackCount.load() == 1; }));
      CHECK_FALSE(receivedImage.load());

      auto retryReceivedImage = rt::test::AsyncTestState<bool>::create(true);
      auto retry =
        loader.requestThumbnail(kMissingResourceId,
                                kPixelSize,
                                [callbackCount, retryReceivedImage](Glib::RefPtr<Gdk::Pixbuf> const& imagePtr)
                                {
                                  retryReceivedImage.set(static_cast<bool>(imagePtr));
                                  callbackCount.increment();
                                });
      REQUIRE(retry);
      REQUIRE(executor.drainUntil([&] { return callbackCount.load() == 2; }));
      CHECK(loadCount.load() == 2);
      CHECK_FALSE(retryReceivedImage.load());

      runtime.requestStop();
      runtime.join();
    }

    SECTION("cancellation escapes without invoking the waiter")
    {
      auto callbackCount = rt::test::AsyncTestState<std::size_t>::create(0);
      auto loadCount = rt::test::AsyncTestState<std::size_t>::create(0);
      auto byteLoader = rt::ResourceByteLoader{runtime, std::bind_front(cancelResourceLoad, loadCount)};
      auto loader = ResourceImageLoader{byteLoader, cache, runtime};

      auto request =
        loader.requestThumbnail(kMissingResourceId,
                                kPixelSize,
                                [callbackCount](Glib::RefPtr<Gdk::Pixbuf> const&) { callbackCount.increment(); });
      REQUIRE(request);
      REQUIRE(loadCount.waitUntil(1));

      runtime.requestStop();
      runtime.join();
      CHECK(callbackCount.load() == 0);
    }
  }

  TEST_CASE("ResourceByteLoader - GTK derivatives share one owner-affine raw resource flight",
            "[gtk][regression][resource-byte][concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto executor = rt::test::QueuedExecutor{};
    auto runtime = async::Runtime{executor, 4};
    auto const ownerThread = std::this_thread::get_id();
    auto release = rt::test::AsyncBarrier{};
    auto loadCount = rt::test::AsyncTestState<std::size_t>::create(0);
    auto const pngBytes = encodePng(makePixbuf(256));
    auto byteLoader =
      rt::ResourceByteLoader{runtime, std::bind_front(loadResourceAfterRelease, loadCount, &release, pngBytes)};
    auto imageCache = ImageCache{200};
    auto imageLoader = ResourceImageLoader{byteLoader, imageCache, runtime};
    auto tempDir = ao::test::TempDir{};
    auto artUrlCache = platform::MprisArtUrlCache{byteLoader, runtime, tempDir.path() / "shared-resource-bytes"};
    constexpr auto kResourceId = ResourceId{8181};
    auto urlCallbackCount = rt::test::AsyncTestState<std::size_t>::create(0);
    auto nonEmptyUrlCount = rt::test::AsyncTestState<std::size_t>::create(0);
    auto callbacksOnOwner = rt::test::AsyncTestState<bool>::create(true);
    auto urlRequest =
      artUrlCache.requestUrl(kResourceId,
                             [ownerThread, callbacksOnOwner, urlCallbackCount, nonEmptyUrlCount](std::string result)
                             {
                               if (std::this_thread::get_id() != ownerThread)
                               {
                                 callbacksOnOwner.set(false);
                               }

                               if (!result.empty())
                               {
                                 nonEmptyUrlCount.increment();
                               }

                               urlCallbackCount.increment();
                             });
    REQUIRE(urlRequest);
    REQUIRE(executor.drainUntil([&] { return loadCount.load() == 1; }));

    auto imageCallbackCount = rt::test::AsyncTestState<std::size_t>::create(0);
    auto nonEmptyImageCount = rt::test::AsyncTestState<std::size_t>::create(0);
    auto const requestImage = [&](std::int32_t const physicalPixelSize)
    {
      return imageLoader.requestThumbnail(kResourceId,
                                          physicalPixelSize,
                                          [ownerThread, callbacksOnOwner, imageCallbackCount, nonEmptyImageCount](
                                            Glib::RefPtr<Gdk::Pixbuf> const& imagePtr)
                                          {
                                            if (std::this_thread::get_id() != ownerThread)
                                            {
                                              callbacksOnOwner.set(false);
                                            }

                                            if (imagePtr)
                                            {
                                              nonEmptyImageCount.increment();
                                            }

                                            imageCallbackCount.increment();
                                          });
    };
    auto smallRequest = requestImage(48);
    auto largeRequest = requestImage(96);
    REQUIRE(smallRequest);
    REQUIRE(largeRequest);
    CHECK(loadCount.load() == 1);

    release.release();
    REQUIRE(executor.drainUntil([&] { return urlCallbackCount.load() == 1 && imageCallbackCount.load() == 2; }));
    CHECK(imageLoader.getThumbnail(kResourceId, 48));
    CHECK(imageLoader.getThumbnail(kResourceId, 96));

    runtime.requestStop();
    runtime.join();
    executor.drain();

    CHECK(callbacksOnOwner.load());
    CHECK(urlCallbackCount.load() == 1);
    CHECK(imageCallbackCount.load() == 2);
    CHECK(nonEmptyUrlCount.load() == 1);
    CHECK(nonEmptyImageCount.load() == 2);
    CHECK(loadCount.load() == 1);
    CHECK(executor.queuedCount() == 0);
  }
} // namespace ao::gtk::test
