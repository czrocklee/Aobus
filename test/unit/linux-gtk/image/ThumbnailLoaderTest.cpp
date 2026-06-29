// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ThumbnailLoader.h"

#include "image/ImageCache.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/linux-gtk/image/ImageTestSupport.h"
#include <ao/CoreIds.h>

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
  TEST_CASE("ThumbnailLoader resolves image sources into pixbuf results", "[gtk][unit][image][thumbnail]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& library = runtime.musicLibrary();
    auto cache = ImageCache{200};
    auto loader = ThumbnailLoader{runtime.library(), cache, runtime.async()};

    constexpr std::int32_t kPixelSize = 48;

    SECTION("request decodes off-thread, populates the cache, and invokes the callback")
    {
      auto const resourceId = writeCoverResource(library, 256);

      auto receivedPtr = Glib::RefPtr<Gdk::Pixbuf>{};
      std::int32_t callbackCount = 0;
      auto request = loader.request(resourceId,
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
      auto const cachedPtr = loader.get(resourceId, kPixelSize);
      REQUIRE(cachedPtr);
      CHECK(cachedPtr.get() == receivedPtr.get());
    }

    SECTION("concurrent requests for the same id coalesce into a single decode")
    {
      auto const resourceId = writeCoverResource(library, 256);

      auto firstPtr = Glib::RefPtr<Gdk::Pixbuf>{};
      auto secondPtr = Glib::RefPtr<Gdk::Pixbuf>{};
      bool firstDone = false;
      bool secondDone = false;

      auto firstRequest = loader.request(resourceId,
                                         kPixelSize,
                                         [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
                                         {
                                           firstPtr = pixbufPtr;
                                           firstDone = true;
                                         });
      auto secondRequest = loader.request(resourceId,
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
      auto const resourceId = writeCoverResource(library, 256);
      auto callbackOrder = std::vector<int>{};
      auto requests = std::vector<ThumbnailLoader::Request>{};

      for (std::int32_t idx = 0; idx < 4; ++idx)
      {
        requests.push_back(loader.request(
          resourceId, kPixelSize, [&, idx](Glib::RefPtr<Gdk::Pixbuf> const&) { callbackOrder.push_back(idx); }));
      }

      REQUIRE(pumpGtkEventsUntil([&] { return callbackOrder.size() == 4; }));
      CHECK(callbackOrder == std::vector<int>{0, 1, 2, 3});
    }

    SECTION("larger requests are not satisfied by smaller in-flight decodes")
    {
      auto const resourceId = writeCoverResource(library, 256);

      auto smallPtr = Glib::RefPtr<Gdk::Pixbuf>{};
      auto largePtr = Glib::RefPtr<Gdk::Pixbuf>{};

      auto smallRequest =
        loader.request(resourceId, 48, [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr) { smallPtr = pixbufPtr; });
      auto largeRequest =
        loader.request(resourceId, 96, [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr) { largePtr = pixbufPtr; });
      REQUIRE(smallRequest);
      REQUIRE(largeRequest);

      REQUIRE(pumpGtkEventsUntil([&] { return smallPtr && largePtr; }));
      CHECK(smallPtr.get() != largePtr.get());
      CHECK(std::max(smallPtr->get_width(), smallPtr->get_height()) <= 48);
      CHECK(std::max(largePtr->get_width(), largePtr->get_height()) >= 96);
      CHECK(loader.get(resourceId, 96));
    }

    SECTION("prefetch warms the cache without a callback")
    {
      auto const resourceId = writeCoverResource(library, 256);

      loader.prefetch(resourceId, kPixelSize);

      REQUIRE(pumpGtkEventsUntil([&] { return static_cast<bool>(loader.get(resourceId, kPixelSize)); }));
      CHECK(loader.get(resourceId, kPixelSize));
    }

    SECTION("prefetch is a no-op for invalid, cached, and already in-flight requests")
    {
      auto const resourceId = writeCoverResource(library, 256);

      loader.prefetch(kInvalidResourceId, kPixelSize);
      CHECK_FALSE(loader.get(kInvalidResourceId, kPixelSize));

      std::int32_t callbackCount = 0;
      auto request = loader.request(resourceId, kPixelSize, [&](Glib::RefPtr<Gdk::Pixbuf> const&) { ++callbackCount; });
      REQUIRE(request);
      loader.prefetch(resourceId, kPixelSize);

      REQUIRE(pumpGtkEventsUntil([&] { return callbackCount == 1; }));
      auto const firstCachedPtr = loader.get(resourceId, kPixelSize);
      REQUIRE(firstCachedPtr);

      loader.prefetch(resourceId, kPixelSize);
      CHECK(loader.get(resourceId, kPixelSize).get() == firstCachedPtr.get());
    }

    SECTION("a cache hit invokes the callback synchronously")
    {
      auto const resourceId = ResourceId{4242};
      cache.put(ImageCacheKey::thumbnail(resourceId, kPixelSize), makePixbuf(kPixelSize));

      bool invokedSynchronously = false;
      auto request = loader.request(resourceId,
                                    kPixelSize,
                                    [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
                                    { invokedSynchronously = static_cast<bool>(pixbufPtr); });

      // No pumping: the hit path must run inline.
      CHECK(invokedSynchronously);
      CHECK_FALSE(request);
    }

    SECTION("request accepts an empty callback and still warms the cache")
    {
      auto const resourceId = writeCoverResource(library, 256);

      auto request = loader.request(resourceId, kPixelSize, ThumbnailLoader::OnThumbnailReady{});
      CHECK_FALSE(request);

      REQUIRE(pumpGtkEventsUntil([&] { return static_cast<bool>(loader.get(resourceId, kPixelSize)); }));
      CHECK(loader.get(resourceId, kPixelSize));
    }

    SECTION("an invalid id reports an empty result and caches nothing")
    {
      bool called = false;
      bool wasEmpty = false;
      auto request = loader.request(kInvalidResourceId,
                                    kPixelSize,
                                    [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
                                    {
                                      called = true;
                                      wasEmpty = !pixbufPtr;
                                    });

      CHECK(called);
      CHECK(wasEmpty);
      CHECK_FALSE(request);
      CHECK_FALSE(loader.get(kInvalidResourceId, kPixelSize));
    }

    SECTION("a missing resource id reports an empty result and clears the in-flight entry")
    {
      std::int32_t callbackCount = 0;
      bool wasEmpty = false;
      auto const missingId = ResourceId{987654};

      auto request = loader.request(missingId,
                                    kPixelSize,
                                    [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
                                    {
                                      ++callbackCount;
                                      wasEmpty = !pixbufPtr;
                                    });
      REQUIRE(request);

      REQUIRE(pumpGtkEventsUntil([&] { return callbackCount == 1; }));
      CHECK(wasEmpty);
      CHECK_FALSE(loader.get(missingId, kPixelSize));

      auto retryRequest =
        loader.request(missingId, kPixelSize, [&](Glib::RefPtr<Gdk::Pixbuf> const&) { ++callbackCount; });
      REQUIRE(retryRequest);
      REQUIRE(pumpGtkEventsUntil([&] { return callbackCount == 2; }));
    }

    SECTION("malformed image bytes report an empty result and are not cached")
    {
      auto const badBytes = std::array{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
      auto const resourceId = writeRawResource(library, std::span<std::byte const>{badBytes});
      std::int32_t callbackCount = 0;
      bool wasEmpty = false;

      auto request = loader.request(resourceId,
                                    kPixelSize,
                                    [&](Glib::RefPtr<Gdk::Pixbuf> const& pixbufPtr)
                                    {
                                      ++callbackCount;
                                      wasEmpty = !pixbufPtr;
                                    });
      REQUIRE(request);

      REQUIRE(pumpGtkEventsUntil([&] { return callbackCount == 1; }));
      CHECK(wasEmpty);
      CHECK_FALSE(loader.get(resourceId, kPixelSize));
    }

    SECTION("destroying a request cancels its callback without cancelling the shared decode")
    {
      auto const resourceId = writeCoverResource(library, 256);
      std::int32_t callbackCount = 0;

      auto request = loader.request(resourceId, kPixelSize, [&](Glib::RefPtr<Gdk::Pixbuf> const&) { ++callbackCount; });
      REQUIRE(request);

      request.reset();

      REQUIRE(pumpGtkEventsUntil([&] { return static_cast<bool>(loader.get(resourceId, kPixelSize)); }));
      CHECK(callbackCount == 0);
    }

    SECTION("destroying the loader cancels pending callbacks")
    {
      auto const resourceId = writeCoverResource(library, 256);
      std::int32_t callbackCount = 0;
      auto request = ThumbnailLoader::Request{};

      {
        auto scopedLoader = ThumbnailLoader{runtime.library(), cache, runtime.async()};
        request =
          scopedLoader.request(resourceId, kPixelSize, [&](Glib::RefPtr<Gdk::Pixbuf> const&) { ++callbackCount; });
        REQUIRE(request);
      }

      CHECK(callbackCount == 0);
      request.reset();

      auto replacementLoader = ThumbnailLoader{runtime.library(), cache, runtime.async()};
      std::int32_t replacementCallbackCount = 0;
      auto replacementRequest = replacementLoader.request(
        resourceId, kPixelSize, [&](Glib::RefPtr<Gdk::Pixbuf> const&) { ++replacementCallbackCount; });
      REQUIRE(replacementRequest);
      REQUIRE(pumpGtkEventsUntil([&] { return replacementCallbackCount == 1; }));
      CHECK(callbackCount == 0);
    }
  }
} // namespace ao::gtk::test
