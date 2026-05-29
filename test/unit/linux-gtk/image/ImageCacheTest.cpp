// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ImageCache.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gdkmm/pixbuf.h>

namespace ao::gtk::test
{
  TEST_CASE("ImageCache - LRU behavior", "[gtk][image][cache]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    SECTION("put and get")
    {
      auto cache = ImageCache{2};
      auto pix1Ptr = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 1, 1);

      cache.put(1, pix1Ptr);
      CHECK(cache.get(1) == pix1Ptr);
      CHECK(cache.get(2) == nullptr);
    }

    SECTION("LRU eviction")
    {
      auto cache = ImageCache{2};
      auto pix1Ptr = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 1, 1);
      auto pix2Ptr = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 1, 1);
      auto pix3Ptr = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 1, 1);

      cache.put(1, pix1Ptr);
      cache.put(2, pix2Ptr);

      // Access 1 to make it MRU
      cache.get(1);

      // Put 3, should evict 2
      cache.put(3, pix3Ptr);

      CHECK(cache.get(1) == pix1Ptr);
      CHECK(cache.get(3) == pix3Ptr);
      CHECK(cache.get(2) == nullptr);
    }

    SECTION("clear")
    {
      auto cache = ImageCache{10};
      auto pix1Ptr = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, 1, 1);
      cache.put(1, pix1Ptr);
      cache.clear();
      CHECK(cache.get(1) == nullptr);
    }
  }
} // namespace ao::gtk::test
