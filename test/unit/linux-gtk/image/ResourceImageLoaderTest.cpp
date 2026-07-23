// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ResourceImageLoader.h"

#include "image/ImageCache.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/linux-gtk/image/ImageTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>

#include <catch2/catch_test_macros.hpp>
#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("ResourceImageLoader - resolves image sources into pixbuf results",
            "[gtk][unit][resource-image][concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto validResourceId = kInvalidResourceId;
    auto malformedResourceId = kInvalidResourceId;
    auto oversizedDimensionResourceId = kInvalidResourceId;
    auto fixture = GtkRuntimeFixture{
      [&](library::MusicLibrary& musicLibrary)
      {
        validResourceId = writeCoverResource(musicLibrary, 256);
        auto const badBytes = std::array{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
        malformedResourceId = writeRawResource(musicLibrary, std::span<std::byte const>{badBytes});
        oversizedDimensionResourceId = writeCoverResource(musicLibrary, makePixbuf(8193, 1));
      }};
    auto& runtime = fixture.runtime();
    auto cache = ImageCache{200};
    auto loader = ResourceImageLoader{runtime.library().taskService(), cache, runtime.async()};

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
        auto scopedLoader = ResourceImageLoader{runtime.library().taskService(), cache, runtime.async()};
        request = scopedLoader.requestThumbnail(
          resourceId, kPixelSize, [&](Glib::RefPtr<Gdk::Pixbuf> const&) { ++callbackCount; });
        REQUIRE(request);
      }

      CHECK(callbackCount == 0);
      request.reset();

      auto replacementLoader = ResourceImageLoader{runtime.library().taskService(), cache, runtime.async()};
      std::int32_t replacementCallbackCount = 0;
      auto replacementRequest = replacementLoader.requestThumbnail(
        resourceId, kPixelSize, [&](Glib::RefPtr<Gdk::Pixbuf> const&) { ++replacementCallbackCount; });
      REQUIRE(replacementRequest);
      REQUIRE(pumpGtkEventsUntil([&] { return replacementCallbackCount == 1; }));
      CHECK(callbackCount == 0);
    }
  }
} // namespace ao::gtk::test
