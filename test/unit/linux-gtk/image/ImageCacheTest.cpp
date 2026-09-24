// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ImageCache.h"

#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gdkmm/pixbuf.h>

namespace ao::gtk::test
{
  TEST_CASE("ImageCache - keeps most-recently-used pixbuf entries", "[gtk][unit][image]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    constexpr auto kResource1 = ResourceId{1};
    constexpr auto kResource2 = ResourceId{2};
    constexpr auto kResource3 = ResourceId{3};

    SECTION("put and get")
    {
      auto cache = ImageCache{2};
      auto pix1Ptr = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 1, 1);

      cache.put(ImageCacheKey::full(kResource1), pix1Ptr);
      CHECK(cache.get(ImageCacheKey::full(kResource1)) == pix1Ptr);
      CHECK(cache.get(ImageCacheKey::full(kResource2)) == nullptr);
    }

    SECTION("LRU eviction")
    {
      auto cache = ImageCache{2};
      auto pix1Ptr = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 1, 1);
      auto pix2Ptr = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 1, 1);
      auto pix3Ptr = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 1, 1);

      cache.put(ImageCacheKey::full(kResource1), pix1Ptr);
      cache.put(ImageCacheKey::full(kResource2), pix2Ptr);

      // Access 1 to make it MRU
      cache.get(ImageCacheKey::full(kResource1));

      // Put 3, should evict 2
      cache.put(ImageCacheKey::full(kResource3), pix3Ptr);

      CHECK(cache.get(ImageCacheKey::full(kResource1)) == pix1Ptr);
      CHECK(cache.get(ImageCacheKey::full(kResource3)) == pix3Ptr);
      CHECK(cache.get(ImageCacheKey::full(kResource2)) == nullptr);
    }

    SECTION("replacement updates pixels and promotes without a read")
    {
      auto cache = ImageCache{2};
      auto firstPtr = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 1, 1);
      auto secondPtr = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 1, 1);
      auto replacementPtr = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 2, 2);
      auto thirdPtr = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 1, 1);
      replacementPtr->fill(0x123456FFU);
      auto* const replacementIdentity = replacementPtr.get();

      cache.put(ImageCacheKey::full(kResource1), firstPtr);
      cache.put(ImageCacheKey::full(kResource2), secondPtr);
      cache.put(ImageCacheKey::full(kResource1), replacementPtr);
      replacementPtr.reset();

      // Replacement itself must promote resource 1. Reading it before this insertion
      // would also promote it and hide a broken replacement branch.
      cache.put(ImageCacheKey::full(kResource3), thirdPtr);

      auto const cachedReplacementPtr = cache.get(ImageCacheKey::full(kResource1));
      REQUIRE(cachedReplacementPtr);
      CHECK(cachedReplacementPtr.get() == replacementIdentity);
      CHECK(cachedReplacementPtr->get_width() == 2);
      CHECK(cachedReplacementPtr->get_height() == 2);
      auto const* const pixels = cachedReplacementPtr->get_pixels();
      REQUIRE(pixels != nullptr);
      CHECK(pixels[0] == 0x12U);
      CHECK(pixels[1] == 0x34U);
      CHECK(pixels[2] == 0x56U);
      CHECK(cache.get(ImageCacheKey::full(kResource2)) == nullptr);
      CHECK(cache.get(ImageCacheKey::full(kResource3)) == thirdPtr);
    }

    SECTION("same resource keeps full-size and thumbnail entries distinct")
    {
      auto cache = ImageCache{3};
      auto fullPtr = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 16, 16);
      auto thumbnailPtr = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 4, 4);
      auto largerThumbnailPtr = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 8, 8);

      cache.put(ImageCacheKey::full(kResource1), fullPtr);
      cache.put(ImageCacheKey::thumbnail(kResource1, 4), thumbnailPtr);
      cache.put(ImageCacheKey::thumbnail(kResource1, 8), largerThumbnailPtr);

      CHECK(cache.get(ImageCacheKey::full(kResource1)) == fullPtr);
      CHECK(cache.get(ImageCacheKey::thumbnail(kResource1, 4)) == thumbnailPtr);
      CHECK(cache.get(ImageCacheKey::thumbnail(kResource1, 8)) == largerThumbnailPtr);
      CHECK(cache.get(ImageCacheKey::thumbnail(kResource1, 16)) == nullptr);
    }

    SECTION("clear")
    {
      auto cache = ImageCache{10};
      auto pix1Ptr = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 1, 1);
      cache.put(ImageCacheKey::full(kResource1), pix1Ptr);
      cache.clear();
      CHECK(cache.get(ImageCacheKey::full(kResource1)) == nullptr);
    }
  }
} // namespace ao::gtk::test
