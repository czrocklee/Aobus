// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ImageWidget.h"

#include "image/ImageCache.h"
#include "image/ResourceImageController.h"
#include "image/ThumbnailLoader.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/linux-gtk/image/ImageTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/projection/TrackDetailProjection.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

namespace ao::gtk::test
{
  TEST_CASE("ImageWidget - renders pixbufs at target and allocated sizes", "[gtk][unit][image]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto widget = ImageWidget{};

    SECTION("initial state has alt text")
    {
      CHECK(widget.get_alternative_text() == "No cover art");
    }

    SECTION("allocated rendering uses current widget size")
    {
      auto const scaleFactor = widget.get_scale_factor();
      auto const sourcePixbufPtr = makePixbuf(200, 200);

      widget.setTargetSize(56);
      widget.setImagePixbuf(sourcePixbufPtr);
      drainGtkEvents();

      auto paintablePtr = widget.get_paintable();
      REQUIRE(paintablePtr);
      CHECK(paintablePtr->get_intrinsic_width() == 56 * scaleFactor);
      CHECK(paintablePtr->get_intrinsic_height() == 56 * scaleFactor);

      auto allocationHost = AllocationHost{widget};
      allocationHost.allocateChild(160, 96);

      widget.setImagePixbuf(sourcePixbufPtr);
      drainGtkEvents();

      REQUIRE(widget.get_width() > 0);
      REQUIRE(widget.get_height() > 0);

      auto const expectedSize = std::min(widget.get_width(), widget.get_height()) * scaleFactor;
      paintablePtr = widget.get_paintable();
      REQUIRE(paintablePtr);
      CHECK(paintablePtr->get_intrinsic_width() == expectedSize);
      CHECK(paintablePtr->get_intrinsic_height() == expectedSize);
    }

    SECTION("force square target follows the allocated short side")
    {
      auto const scaleFactor = widget.get_scale_factor();
      auto const sourcePixbufPtr = makePixbuf(400, 300);

      widget.setTargetSize(48);
      widget.setForceSquareTarget(true);
      widget.setImagePixbuf(sourcePixbufPtr);
      drainGtkEvents();

      auto paintablePtr = widget.get_paintable();
      REQUIRE(paintablePtr);
      CHECK(paintablePtr->get_intrinsic_width() == 48 * scaleFactor);
      CHECK(paintablePtr->get_intrinsic_height() == 48 * scaleFactor);

      auto allocationHost = AllocationHost{widget};
      allocationHost.allocateChild(64, 80);

      widget.setImagePixbuf(sourcePixbufPtr);
      drainGtkEvents();

      auto const expectedSize = std::min(widget.get_width(), widget.get_height()) * scaleFactor;
      paintablePtr = widget.get_paintable();
      REQUIRE(paintablePtr);
      CHECK(paintablePtr->get_intrinsic_width() == expectedSize);
      CHECK(paintablePtr->get_intrinsic_height() == expectedSize);
    }

    SECTION("new pixbuf refreshes rendered image")
    {
      auto const scaleFactor = widget.get_scale_factor();
      auto const smallPixbufPtr = makePixbuf(40, 40);
      auto const largePixbufPtr = makePixbuf(200, 200);

      widget.setTargetSize(56);
      widget.setImagePixbuf(smallPixbufPtr);
      drainGtkEvents();

      auto paintablePtr = widget.get_paintable();
      REQUIRE(paintablePtr);
      CHECK(paintablePtr->get_intrinsic_width() == 40);
      CHECK(paintablePtr->get_intrinsic_height() == 40);

      widget.setImagePixbuf(largePixbufPtr);
      drainGtkEvents();

      paintablePtr = widget.get_paintable();
      REQUIRE(paintablePtr);
      CHECK(paintablePtr->get_intrinsic_width() == 56 * scaleFactor);
      CHECK(paintablePtr->get_intrinsic_height() == 56 * scaleFactor);
    }

    SECTION("resize settles to a full-quality render at the final size")
    {
      auto const scaleFactor = widget.get_scale_factor();
      // A source far larger than any target so the fit always downscales to the
      // requested size exactly (no clamping to the source dimensions).
      auto const sourcePixbufPtr = makePixbuf(2000, 2000);

      // First paint at a known size: a fresh source goes straight to full quality.
      widget.setTargetSize(56);
      widget.setImagePixbuf(sourcePixbufPtr);
      drainGtkEvents();

      auto const firstPaintablePtr = widget.get_paintable();
      REQUIRE(firstPaintablePtr);
      CHECK(firstPaintablePtr->get_intrinsic_width() == 56 * scaleFactor);

      // Changing the render target on the same source is a resize step: it paints
      // immediately (cheap interim filter) at the new size...
      widget.setTargetSize(96);
      drainGtkEvents();

      auto const interimPaintablePtr = widget.get_paintable();
      REQUIRE(interimPaintablePtr);
      REQUIRE(interimPaintablePtr.get() != firstPaintablePtr.get());
      CHECK(interimPaintablePtr->get_intrinsic_width() == 96 * scaleFactor);

      // ...then, once the settle window elapses, it is replaced by a fresh
      // full-quality re-render: a different texture object at the same size.
      REQUIRE(pumpUntil([&] { return widget.get_paintable().get() != interimPaintablePtr.get(); }));

      auto const settledPaintablePtr = widget.get_paintable();
      REQUIRE(settledPaintablePtr);
      CHECK(settledPaintablePtr->get_intrinsic_width() == 96 * scaleFactor);
      CHECK(settledPaintablePtr->get_intrinsic_height() == 96 * scaleFactor);
    }

    SECTION("resize settle timer follows the last target size")
    {
      auto const scaleFactor = widget.get_scale_factor();
      auto const sourcePixbufPtr = makePixbuf(2000, 2000);

      widget.setTargetSize(56);
      widget.setImagePixbuf(sourcePixbufPtr);
      drainGtkEvents();

      widget.setTargetSize(96);
      drainGtkEvents();
      widget.setTargetSize(64);
      drainGtkEvents();

      auto const interimPaintablePtr = widget.get_paintable();
      REQUIRE(interimPaintablePtr);
      CHECK(interimPaintablePtr->get_intrinsic_width() == 64 * scaleFactor);

      REQUIRE(pumpUntil([&] { return widget.get_paintable().get() != interimPaintablePtr.get(); }));

      auto const settledPaintablePtr = widget.get_paintable();
      REQUIRE(settledPaintablePtr);
      CHECK(settledPaintablePtr->get_intrinsic_width() == 64 * scaleFactor);
      CHECK(settledPaintablePtr->get_intrinsic_height() == 64 * scaleFactor);
    }

    SECTION("new source during resize settle renders the new image")
    {
      auto const scaleFactor = widget.get_scale_factor();
      auto const firstPixbufPtr = makePixbuf(2000, 2000);
      auto const secondPixbufPtr = makePixbuf(3000, 3000);

      widget.setTargetSize(56);
      widget.setImagePixbuf(firstPixbufPtr);
      drainGtkEvents();

      widget.setTargetSize(96);
      drainGtkEvents();

      auto const interimPaintablePtr = widget.get_paintable();
      REQUIRE(interimPaintablePtr);

      widget.setImagePixbuf(secondPixbufPtr);
      drainGtkEvents();

      auto const newSourcePaintablePtr = widget.get_paintable();
      REQUIRE(newSourcePaintablePtr);
      CHECK(newSourcePaintablePtr.get() != interimPaintablePtr.get());
      CHECK(newSourcePaintablePtr->get_intrinsic_width() == 96 * scaleFactor);
      CHECK(newSourcePaintablePtr->get_intrinsic_height() == 96 * scaleFactor);
    }

    SECTION("small target growth refreshes undersized render")
    {
      auto const scaleFactor = widget.get_scale_factor();
      auto const sourcePixbufPtr = makePixbuf(200, 200);

      widget.setTargetSize(56);
      widget.setImagePixbuf(sourcePixbufPtr);
      drainGtkEvents();

      auto paintablePtr = widget.get_paintable();
      REQUIRE(paintablePtr);
      CHECK(paintablePtr->get_intrinsic_width() == 56 * scaleFactor);
      CHECK(paintablePtr->get_intrinsic_height() == 56 * scaleFactor);

      widget.setTargetSize(58);
      drainGtkEvents();

      paintablePtr = widget.get_paintable();
      REQUIRE(paintablePtr);
      CHECK(paintablePtr->get_intrinsic_width() == 58 * scaleFactor);
      CHECK(paintablePtr->get_intrinsic_height() == 58 * scaleFactor);
    }
  }

  TEST_CASE("ResourceImageController - binds placeholder and loaded image states", "[gtk][unit][image]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto imageCache = ImageCache{200};

    SECTION("loads a cached resource into the widget")
    {
      auto const resourceId = ResourceId{42};
      imageCache.put(ImageCacheKey::full(resourceId), makePixbuf(80, 80));

      auto widget = ImageWidget{};
      auto controller = ResourceImageController{widget, runtime.library(), imageCache};

      widget.setTargetSize(56);
      controller.load(resourceId);
      drainGtkEvents();

      auto const paintablePtr = widget.get_paintable();
      REQUIRE(paintablePtr);
      CHECK(paintablePtr->get_intrinsic_width() == 56 * widget.get_scale_factor());
      CHECK(paintablePtr->get_intrinsic_height() == 56 * widget.get_scale_factor());
    }

    SECTION("binds to a projection and ignores missing resources")
    {
      auto mockProjPtr = std::make_unique<ManualTrackDetailMock>();
      auto* mock = mockProjPtr.get();

      auto widget = ImageWidget{};
      auto controller = ResourceImageController{widget, runtime.library(), imageCache};

      controller.bindToDetailProjection(std::move(mockProjPtr));

      auto snapshot = rt::TrackDetailSnapshot{};
      snapshot.selectionKind = rt::SelectionKind::Single;
      snapshot.trackIds = {TrackId{1}};
      snapshot.singleCoverArtId = ResourceId{123};

      mock->emit(snapshot);
      drainGtkEvents();

      CHECK_FALSE(widget.get_paintable());
    }
  }

  TEST_CASE("ResourceImageController - async thumbnail mode updates the widget image", "[gtk][unit][image]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto& library = runtime.musicLibrary();
    auto thumbnailCache = ImageCache{200};
    auto loader = ThumbnailLoader{runtime.library(), thumbnailCache, runtime.async()};

    constexpr std::int32_t kLogicalSize = 48;

    SECTION("cache miss decodes off-thread at scale and populates the cache")
    {
      // A large square source so we can prove the cached result is downscaled.
      auto const resourceId = writeCoverResource(library, makePixbuf(256, 256));

      auto widget = ImageWidget{};
      auto controller = ResourceImageController{widget, runtime.library(), thumbnailCache};
      controller.enableThumbnailMode(loader, kLogicalSize);
      controller.load(resourceId);

      auto const scaleFactor = widget.get_scale_factor();
      auto const expectedSide = kLogicalSize * scaleFactor;

      auto const physicalSize =
        std::max(1, static_cast<std::int32_t>(std::ceil(static_cast<double>(kLogicalSize) * widget.displayScale())));
      REQUIRE(pumpUntil([&] { return static_cast<bool>(loader.get(resourceId, physicalSize)); }));

      auto const cachedPtr = loader.get(resourceId, physicalSize);
      REQUIRE(cachedPtr);
      // Decode-at-scale: the stored thumbnail is bounded by the logical size
      // times the display scale, never the full 256px source.
      CHECK(cachedPtr->get_width() <= expectedSide);
      CHECK(cachedPtr->get_height() <= expectedSide);
      CHECK(cachedPtr->get_width() < 256);

      REQUIRE(pumpUntil([&] { return static_cast<bool>(widget.get_paintable()); }));
      CHECK(widget.get_paintable());
    }

    SECTION("cache hit renders synchronously without touching the database")
    {
      // Resource id that does not exist in the database; a synchronous hit must
      // not require any decode, proving the fast path bypasses the worker.
      auto const resourceId = ResourceId{9001};
      thumbnailCache.put(ImageCacheKey::thumbnail(resourceId, kLogicalSize), makePixbuf(kLogicalSize, kLogicalSize));

      auto widget = ImageWidget{};
      auto controller = ResourceImageController{widget, runtime.library(), thumbnailCache};
      controller.enableThumbnailMode(loader, kLogicalSize);
      controller.load(resourceId);
      drainGtkEvents();

      CHECK(widget.get_paintable());
    }

    SECTION("invalid resource id clears the image")
    {
      auto widget = ImageWidget{};
      auto controller = ResourceImageController{widget, runtime.library(), thumbnailCache};
      controller.enableThumbnailMode(loader, kLogicalSize);
      controller.load(kInvalidResourceId);
      drainGtkEvents();

      CHECK_FALSE(widget.get_paintable());
    }

    SECTION("destroying a widget mid-decode is safe")
    {
      auto const resourceId = writeCoverResource(library, makePixbuf(256, 256));

      {
        auto widget = ImageWidget{};
        auto controller = ResourceImageController{widget, runtime.library(), thumbnailCache};
        controller.enableThumbnailMode(loader, kLogicalSize);
        controller.load(resourceId);
        // Leave the scope immediately: the decode is likely still in flight on a
        // worker thread. The shared loader outlives the widget and still completes
        // the decode, but the controller's request handle must cancel the UI
        // callback so it never touches the destroyed widget.
      }

      // The shared loader still salvages the decode into the cache, while the
      // controller's destroyed request handle prevents the callback from touching it.
      REQUIRE(pumpUntil([&] { return static_cast<bool>(loader.get(resourceId, kLogicalSize)); }));

      // The runtime remains usable afterwards.
      auto widget = ImageWidget{};
      auto controller = ResourceImageController{widget, runtime.library(), thumbnailCache};
      controller.enableThumbnailMode(loader, kLogicalSize);
      controller.load(resourceId);
      REQUIRE(pumpUntil([&] { return static_cast<bool>(widget.get_paintable()); }));
      CHECK(widget.get_paintable());
    }
  }
} // namespace ao::gtk::test
