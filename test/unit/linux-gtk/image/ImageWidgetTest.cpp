// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ImageWidget.h"

#include "image/CoverArtView.h"
#include "image/ImageCache.h"
#include "image/ResourceImageController.h"
#include "image/ResourceImageLoader.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "test/unit/linux-gtk/image/ImageTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/resource/ResourceByteLoader.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/drawingarea.h>
#include <gtkmm/label.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

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

  TEST_CASE("CoverArtView - renders every placeholder style and yields to real artwork",
            "[gtk][unit][image][cover-art]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto widget = CoverArtView{};
    auto const identity = uimodel::makeCoverArtPlaceholderIdentity(std::array<std::string_view, 1>{"Synthetic Sun"});

    for (auto const style : {uimodel::CoverArtPlaceholderStyle::Monogram,
                             uimodel::CoverArtPlaceholderStyle::Note,
                             uimodel::CoverArtPlaceholderStyle::Vinyl,
                             uimodel::CoverArtPlaceholderStyle::Equalizer,
                             uimodel::CoverArtPlaceholderStyle::Soul})
    {
      widget.showPlaceholder(uimodel::makeCoverArtPlaceholderPresentation(style, identity));
      CHECK(widget.showingPlaceholder());
      CHECK(widget.placeholderPresentation().style == style);
      CHECK_FALSE(widget.hasImage());
    }

    widget.setTargetSize(56);
    widget.setImagePixbuf(makePixbuf(80, 80));
    drainGtkEvents();

    CHECK_FALSE(widget.showingPlaceholder());
    CHECK(widget.hasImage());

    widget.clearImage();
    CHECK_FALSE(widget.showingPlaceholder());
    CHECK_FALSE(widget.hasImage());
  }

  TEST_CASE("CoverArtView - compact monogram reduces two-scalar text within the cover",
            "[gtk][regression][cover-art][geometry]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto widget = CoverArtView{};
    auto const regularIdentity =
      uimodel::makeCoverArtPlaceholderIdentity(std::array<std::string_view, 1>{"Synthetic Sun"});
    widget.setTargetSize(48);
    widget.showPlaceholder(
      uimodel::makeCoverArtPlaceholderPresentation(uimodel::CoverArtPlaceholderStyle::Monogram, regularIdentity));

    auto allocationHost = AllocationHost{widget};
    allocationHost.allocateChild(48, 48);
    drainGtkEvents();

    auto* const label = findWidgetByClass<Gtk::Label>(widget, "ao-cover-monogram");
    REQUIRE(label != nullptr);
    auto const layoutPtr = label->get_layout();
    REQUIRE(layoutPtr);
    std::int32_t textWidth = 0;
    std::int32_t regularTextHeight = 0;
    layoutPtr->get_pixel_size(textWidth, regularTextHeight);

    auto const compactPresentation =
      uimodel::makeCoverArtPlaceholderPresentation(uimodel::CoverArtPlaceholderStyle::Monogram,
                                                   uimodel::CoverArtPlaceholderIdentity{
                                                     .primaryText = "2023",
                                                     .optMonogram = "23",
                                                   });
    REQUIRE(compactPresentation.monogramSize == uimodel::CoverArtPlaceholderMonogramSize::Compact);
    widget.showPlaceholder(compactPresentation);
    allocationHost.allocateChild(48, 48);
    drainGtkEvents();

    auto const compactLayoutPtr = label->get_layout();
    REQUIRE(compactLayoutPtr);
    std::int32_t compactTextWidth = 0;
    std::int32_t compactTextHeight = 0;
    compactLayoutPtr->get_pixel_size(compactTextWidth, compactTextHeight);

    CHECK(compactTextHeight < regularTextHeight);
    CHECK(label->get_width() >= compactTextWidth);
    CHECK(label->get_height() >= compactTextHeight);
  }

  TEST_CASE("CoverArtView - vinyl decoration follows the cover allocation", "[gtk][regression][cover-art][geometry]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto widget = CoverArtView{};
    widget.setTargetSize(48);
    widget.showPlaceholder(uimodel::makeCoverArtPlaceholderPresentation(uimodel::CoverArtPlaceholderStyle::Vinyl,
                                                                        uimodel::CoverArtPlaceholderIdentity{
                                                                          .primaryText = "Synthetic Sun",
                                                                        }));

    auto allocationHost = AllocationHost{widget};
    allocationHost.allocateChild(48, 48);
    drainGtkEvents();

    auto* const decoration = findWidgetByClass<Gtk::DrawingArea>(widget, "ao-cover-vinyl-decoration");
    REQUIRE(decoration != nullptr);
    CHECK(decoration->get_visible());
    CHECK(decoration->get_width() == widget.get_width());
    CHECK(decoration->get_height() == widget.get_height());

    widget.setTargetSize(180);
    allocationHost.allocateChild(180, 180);
    drainGtkEvents();

    CHECK(decoration->get_visible());
    CHECK(decoration->get_width() == widget.get_width());
    CHECK(decoration->get_height() == widget.get_height());
  }

  TEST_CASE("ResourceImageController - binds placeholder and loaded image states", "[gtk][unit][image]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fullResourceId = kInvalidResourceId;
    auto fixture = GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                                     { fullResourceId = writeCoverResource(musicLibrary, 128); }};
    auto& runtime = fixture.runtime();
    auto imageCache = ImageCache{200};
    auto byteLoader = rt::ResourceByteLoader{runtime};
    auto loader = ResourceImageLoader{byteLoader, imageCache, runtime.async()};

    SECTION("loads a cached resource into the widget")
    {
      auto const resourceId = ResourceId{42};
      imageCache.put(ImageCacheKey::full(resourceId), makePixbuf(80, 80));
      auto availability = std::vector<bool>{};

      auto widget = CoverArtView{};
      auto controller = ResourceImageController{
        widget, loader, [&availability](bool const available) { availability.push_back(available); }};

      widget.setTargetSize(56);
      controller.load(resourceId);
      drainGtkEvents();

      CHECK(widget.hasImage());
      CHECK(controller.imageAvailable());
      CHECK(availability == std::vector{true});

      controller.load(kInvalidResourceId);

      CHECK(widget.showingPlaceholder());
      CHECK_FALSE(controller.imageAvailable());
      CHECK(availability == std::vector{true, false});

      controller.load(resourceId);

      CHECK(controller.imageAvailable());
      CHECK(availability == std::vector{true, false, true});

      // Reloading an already available resource reports no transition, so observers that
      // need the current state after every load must read it rather than latch the callback.
      controller.load(resourceId);

      CHECK(controller.imageAvailable());
      CHECK(availability == std::vector{true, false, true});
    }

    SECTION("full-size cache miss clears the placeholder and completes asynchronously")
    {
      auto widget = CoverArtView{};
      auto controller = ResourceImageController{widget, loader};
      widget.setTargetSize(56);

      controller.load(kInvalidResourceId);
      drainGtkEvents();
      REQUIRE(widget.showingPlaceholder());

      controller.load(fullResourceId);

      CHECK_FALSE(widget.showingPlaceholder());
      CHECK_FALSE(widget.hasImage());
      REQUIRE(pumpUntil([&] { return widget.hasImage(); }));
      CHECK(loader.getFull(fullResourceId));
    }

    SECTION("missing valid resource leaves the widget empty")
    {
      auto const missingId = ResourceId{987654};
      auto widget = CoverArtView{};
      auto controller = ResourceImageController{widget, loader};
      widget.setTargetSize(56);

      controller.load(kInvalidResourceId);
      drainGtkEvents();
      REQUIRE(widget.showingPlaceholder());

      controller.load(missingId);
      bool missingSettled = false;
      [[maybe_unused]] auto const settlementProbe =
        loader.requestFull(missingId, [&](auto const&) { missingSettled = true; });

      CHECK_FALSE(widget.hasImage());
      REQUIRE(pumpUntil([&] { return missingSettled; }));
      CHECK_FALSE(widget.hasImage());
    }

    SECTION("stale full-size completion cannot clear a newer cached image")
    {
      auto const missingId = ResourceId{987654};
      auto const cachedId = ResourceId{4242};
      imageCache.put(ImageCacheKey::full(cachedId), makePixbuf(96));
      auto widget = CoverArtView{};
      auto controller = ResourceImageController{widget, loader};
      widget.setTargetSize(56);

      controller.load(missingId);
      bool missingSettled = false;
      [[maybe_unused]] auto const settlementProbe =
        loader.requestFull(missingId, [&](auto const&) { missingSettled = true; });
      controller.load(cachedId);

      REQUIRE(pumpUntil([&] { return widget.hasImage(); }));
      REQUIRE(pumpUntil([&] { return missingSettled; }));
      drainGtkEvents();
      CHECK(widget.hasImage());
    }
  }

  TEST_CASE("ResourceImageController - async thumbnail mode updates the widget image",
            "[gtk][unit][image][concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto thumbnailResourceId = kInvalidResourceId;
    auto fixture = GtkRuntimeFixture{[&](library::MusicLibrary& musicLibrary)
                                     { thumbnailResourceId = writeCoverResource(musicLibrary, makePixbuf(256, 256)); }};
    auto& runtime = fixture.runtime();
    auto thumbnailCache = ImageCache{200};
    auto byteLoader = rt::ResourceByteLoader{runtime};
    auto loader = ResourceImageLoader{byteLoader, thumbnailCache, runtime.async()};

    constexpr std::int32_t kLogicalSize = 48;

    SECTION("cache miss decodes off-thread at scale and populates the cache")
    {
      // A large square source so we can prove the cached result is downscaled.
      auto const resourceId = thumbnailResourceId;

      auto widget = CoverArtView{};
      auto controller = ResourceImageController{widget, loader};
      controller.enableThumbnailMode(kLogicalSize);
      controller.load(resourceId);

      auto const scaleFactor = widget.get_scale_factor();
      auto const expectedSide = kLogicalSize * scaleFactor;

      auto const physicalSize =
        std::max(1, static_cast<std::int32_t>(std::ceil(static_cast<double>(kLogicalSize) * widget.displayScale())));
      REQUIRE(pumpUntil([&] { return static_cast<bool>(loader.getThumbnail(resourceId, physicalSize)); }));

      auto const cachedPtr = loader.getThumbnail(resourceId, physicalSize);
      REQUIRE(cachedPtr);
      // Decode-at-scale: the stored thumbnail is bounded by the logical size
      // times the display scale, never the full 256px source.
      CHECK(cachedPtr->get_width() <= expectedSide);
      CHECK(cachedPtr->get_height() <= expectedSide);
      CHECK(cachedPtr->get_width() < 256);

      REQUIRE(pumpUntil([&] { return widget.hasImage(); }));
      CHECK(widget.hasImage());
    }

    SECTION("cache hit renders synchronously without touching the database")
    {
      // Resource id that does not exist in the database; a synchronous hit must
      // not require any decode, proving the fast path bypasses the worker.
      auto const resourceId = ResourceId{9001};
      thumbnailCache.put(ImageCacheKey::thumbnail(resourceId, kLogicalSize), makePixbuf(kLogicalSize, kLogicalSize));

      auto widget = CoverArtView{};
      auto controller = ResourceImageController{widget, loader};
      controller.enableThumbnailMode(kLogicalSize);
      controller.load(resourceId);
      drainGtkEvents();

      CHECK(widget.hasImage());
    }

    SECTION("invalid resource id displays the no-cover placeholder")
    {
      auto widget = CoverArtView{};
      auto controller = ResourceImageController{widget, loader};
      widget.setTargetSize(kLogicalSize);
      controller.enableThumbnailMode(kLogicalSize);
      controller.load(kInvalidResourceId);
      drainGtkEvents();

      CHECK(widget.showingPlaceholder());
      CHECK(widget.placeholderPresentation().style == uimodel::CoverArtPlaceholderStyle::Note);
    }

    SECTION("destroying a widget mid-decode is safe")
    {
      auto const resourceId = thumbnailResourceId;

      {
        auto widget = CoverArtView{};
        auto controller = ResourceImageController{widget, loader};
        controller.enableThumbnailMode(kLogicalSize);
        controller.load(resourceId);
        // Leave the scope immediately: the decode is likely still in flight on a
        // worker thread. The shared loader outlives the widget and still completes
        // the decode, but the controller's request handle must cancel the UI
        // callback so it never touches the destroyed widget.
      }

      // The shared loader still salvages the decode into the cache, while the
      // controller's destroyed request handle prevents the callback from touching it.
      REQUIRE(pumpUntil([&] { return static_cast<bool>(loader.getThumbnail(resourceId, kLogicalSize)); }));

      // The runtime remains usable afterwards.
      auto widget = CoverArtView{};
      auto controller = ResourceImageController{widget, loader};
      controller.enableThumbnailMode(kLogicalSize);
      controller.load(resourceId);
      REQUIRE(pumpUntil([&] { return widget.hasImage(); }));
      CHECK(widget.hasImage());
    }
  }
} // namespace ao::gtk::test
