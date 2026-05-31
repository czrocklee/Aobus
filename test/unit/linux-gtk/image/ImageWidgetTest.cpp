// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "image/ImageWidget.h"

#include "image/ImageCache.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ProjectionTypes.h>

#include <catch2/catch_test_macros.hpp>
#include <gdkmm/pixbuf.h>
#include <gdkmm/rectangle.h>
#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

namespace ao::gtk::test
{
  namespace
  {
    Glib::RefPtr<Gdk::Pixbuf> makePixbuf(std::int32_t width, std::int32_t height)
    {
      return Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, width, height);
    }

    class AllocationHost final : public Gtk::Widget
    {
    public:
      explicit AllocationHost(Gtk::Widget& child)
        : _childPtr{&child}
      {
        _childPtr->set_parent(*this);
      }

      ~AllocationHost() override
      {
        if (_childPtr != nullptr)
        {
          _childPtr->unparent();
        }
      }

      AllocationHost(AllocationHost const&) = delete;
      AllocationHost& operator=(AllocationHost const&) = delete;
      AllocationHost(AllocationHost&&) = delete;
      AllocationHost& operator=(AllocationHost&&) = delete;

      void allocateChild(std::int32_t width, std::int32_t height)
      {
        _width = width;
        _height = height;

        std::int32_t minimum = 0;
        std::int32_t natural = 0;
        std::int32_t minimumBaseline = -1;
        std::int32_t naturalBaseline = -1;

        measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, minimumBaseline, naturalBaseline);
        measure(Gtk::Orientation::VERTICAL, width, minimum, natural, minimumBaseline, naturalBaseline);

        auto allocation = Gtk::Allocation{};
        allocation.set_x(0);
        allocation.set_y(0);
        allocation.set_width(width);
        allocation.set_height(height);

        size_allocate(allocation, -1);
      }

    protected:
      Gtk::SizeRequestMode get_request_mode_vfunc() const override { return Gtk::SizeRequestMode::CONSTANT_SIZE; }

      void measure_vfunc(Gtk::Orientation orientation,
                         int /*forSize*/,
                         int& minimum,
                         int& natural,
                         int& minimumBaseline,
                         int& naturalBaseline) const override
      {
        minimum = orientation == Gtk::Orientation::HORIZONTAL ? _width : _height;
        natural = minimum;
        minimumBaseline = -1;
        naturalBaseline = -1;
      }

      void size_allocate_vfunc(int width, int height, int /*baseline*/) override
      {
        if (_childPtr == nullptr)
        {
          return;
        }

        std::int32_t minimum = 0;
        std::int32_t natural = 0;
        std::int32_t minimumBaseline = -1;
        std::int32_t naturalBaseline = -1;
        _childPtr->measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, minimumBaseline, naturalBaseline);
        _childPtr->measure(Gtk::Orientation::VERTICAL, width, minimum, natural, minimumBaseline, naturalBaseline);

        auto allocation = Gtk::Allocation{};
        allocation.set_x(0);
        allocation.set_y(0);
        allocation.set_width(width);
        allocation.set_height(height);

        _childPtr->size_allocate(allocation, -1);
      }

    private:
      Gtk::Widget* _childPtr = nullptr;
      std::int32_t _width = 0;
      std::int32_t _height = 0;
    };
  } // namespace

  TEST_CASE("ImageWidget - rendering math", "[gtk][image]")
  {
    using namespace ao::gtk::detail;

    SECTION("fitSourceIntoTarget - aspect ratio and upscaling")
    {
      // Source 1000x500 (landscape), target 200x200 -> 200x100
      CHECK(fitSourceIntoTarget({1000, 500}, {200, 200}).width == 200);
      CHECK(fitSourceIntoTarget({1000, 500}, {200, 200}).height == 100);

      // Source 500x1000 (portrait), target 200x200 -> 100x200
      CHECK(fitSourceIntoTarget({500, 1000}, {200, 200}).width == 100);
      CHECK(fitSourceIntoTarget({500, 1000}, {200, 200}).height == 200);

      // Source 40x40 (small), target 80x80 -> 40x40 (no upscale)
      CHECK(fitSourceIntoTarget({40, 40}, {80, 80}).width == 40);
      CHECK(fitSourceIntoTarget({40, 40}, {80, 80}).height == 40);

      // Source 100x100 (square), target 50x50 -> 50x50
      CHECK(fitSourceIntoTarget({100, 100}, {50, 50}).width == 50);
      CHECK(fitSourceIntoTarget({100, 100}, {50, 50}).height == 50);
    }

    SECTION("shouldRefresh - thresholds")
    {
      auto const current = RenderTarget{.width = 100, .height = 100};

      // No change
      CHECK_FALSE(shouldRefresh(current, {100, 100}));

      // 4px jitter (below 5% threshold which is 5px for 100px)
      CHECK_FALSE(shouldRefresh(current, {104, 100}));
      CHECK_FALSE(shouldRefresh(current, {100, 104}));

      // 5px change (at 5% threshold)
      CHECK(shouldRefresh(current, {105, 100}));
      CHECK(shouldRefresh(current, {100, 105}));

      // Small widget (20x20, 5% is 1px, floor is 2px)
      auto const small = RenderTarget{.width = 20, .height = 20};
      CHECK_FALSE(shouldRefresh(small, {21, 20}));
      CHECK(shouldRefresh(small, {22, 20}));

      // Larger widget (400x400, 5% is 20)
      auto const large = RenderTarget{.width = 400, .height = 400};
      CHECK_FALSE(shouldRefresh(large, {419, 419}));
      CHECK(shouldRefresh(large, {420, 400}));

      // Non-round 5% uses the ceiling so changes below 5% do not refresh.
      auto const odd = RenderTarget{.width = 101, .height = 101};
      CHECK_FALSE(shouldRefresh(odd, {106, 101}));
      CHECK(shouldRefresh(odd, {107, 101}));
    }
  }

  TEST_CASE("ImageWidget - basic functionality", "[gtk][image]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto imageCache = ImageCache{200};

    auto widget = ImageWidget{runtime.musicLibrary(), imageCache};

    SECTION("initial state has alt text")
    {
      CHECK(widget.get_alternative_text() == "No cover art");
    }

    SECTION("bind to projection updates image")
    {
      auto mockProjPtr = std::make_unique<ManualTrackDetailMock>();
      auto* mockRawPtr = mockProjPtr.get();

      auto snapshot = rt::TrackDetailSnapshot{};
      snapshot.selectionKind = rt::SelectionKind::Single;
      snapshot.trackIds = {TrackId{1}};
      snapshot.singleCoverArtId = ResourceId{123};

      widget.bindToDetailProjection(std::move(mockProjPtr));

      mockRawPtr->emit(snapshot);
      drainGtkEvents();

      // Since we don't have a real resource with ID 123, it won't actually load an image,
      // but it covers the branch in onDetailSnapshot.
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
  }
} // namespace ao::gtk::test
