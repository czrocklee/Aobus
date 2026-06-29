// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/image/ImageCache.h"
#include "app/linux-gtk/layout/runtime/ComponentTooltipController.h"
#include "app/linux-gtk/layout/runtime/ILayoutComponent.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/picture.h>
#include <gtkmm/popover.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  namespace
  {
    class StaticWidgetComponent final : public ILayoutComponent
    {
    public:
      explicit StaticWidgetComponent(Gtk::Widget& widget)
        : _widget{widget}
      {
      }

      Gtk::Widget& widget() override { return _widget; }

    private:
      Gtk::Widget& _widget;
    };

    Gtk::Popover* findPopoverChild(Gtk::Widget& widget)
    {
      for (auto* child = widget.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        if (auto* const popover = dynamic_cast<Gtk::Popover*>(child); popover != nullptr)
        {
          return popover;
        }
      }

      return nullptr;
    }
  } // namespace

  TEST_CASE("playback.image applies declarative image properties", "[gtk][unit][image]")
  {
    auto fixture = LayoutRuntimeFixture{"io.github.aobus.playback_image_test"};
    auto imageCachePtr = std::make_unique<ImageCache>(10);
    auto& ctx = fixture.context();
    ctx.detail.imageCache = imageCachePtr.get();

    SECTION("default image has no extra styling")
    {
      auto const node = LayoutNode{.type = "playback.image"};
      auto const compPtr = fixture.components().create(ctx, node);

      REQUIRE(compPtr != nullptr);
      auto& widget = compPtr->widget();

      auto* const button = dynamic_cast<Gtk::Button*>(&widget);
      REQUIRE(button != nullptr);
      auto* const picture = button->get_child();
      REQUIRE(picture != nullptr);

      CHECK_FALSE(picture->has_css_class("ao-nowplaying-image-thumb"));

      std::int32_t width = -1;
      std::int32_t height = -1;
      picture->get_size_request(width, height);
      CHECK(width == -1);
      CHECK(height == -1);
    }

    SECTION("declarative properties control size and opacity")
    {
      auto node = LayoutNode{.type = "playback.image"};
      node.props["targetSize"] = LayoutValue{static_cast<std::int64_t>(60)};
      node.props["forceSquare"] = LayoutValue{true};
      node.props["opacity"] = LayoutValue{std::string{"0.5"}};
      auto const compPtr = fixture.components().create(ctx, node);

      REQUIRE(compPtr != nullptr);
      auto& widget = compPtr->widget();

      auto* const button = dynamic_cast<Gtk::Button*>(&widget);
      REQUIRE(button != nullptr);
      auto* const slot = button->get_child();
      REQUIRE(slot != nullptr);
      auto* const picture = dynamic_cast<Gtk::Picture*>(slot->get_first_child());
      REQUIRE(picture != nullptr);

      CHECK(button->get_overflow() == Gtk::Overflow::HIDDEN);
      CHECK(picture->get_overflow() == Gtk::Overflow::HIDDEN);
      CHECK_FALSE(widget.get_hexpand());
      CHECK_FALSE(widget.get_vexpand());

      std::int32_t buttonWidth = 0;
      std::int32_t buttonHeight = 0;
      button->get_size_request(buttonWidth, buttonHeight);
      CHECK(buttonWidth == -1);
      CHECK(buttonHeight == -1);

      std::int32_t width = 0;
      std::int32_t height = 0;
      picture->get_size_request(width, height);
      CHECK(width == -1);
      CHECK(height == -1);

      std::int32_t minimum = -1;
      std::int32_t natural = -1;
      std::int32_t minimumBaseline = -1;
      std::int32_t naturalBaseline = -1;
      slot->measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, minimumBaseline, naturalBaseline);
      CHECK(minimum == 60);
      CHECK(natural == 60);

      slot->measure(Gtk::Orientation::VERTICAL, 64, minimum, natural, minimumBaseline, naturalBaseline);
      CHECK(minimum == 0);
      CHECK(natural == 0);

      slot->size_allocate(Gtk::Allocation{0, 0, 60, 64}, -1);
      CHECK(picture->get_width() == 60);
      CHECK(picture->get_height() == 60);
      CHECK(button->get_opacity() == Catch::Approx{0.5}.margin(0.01));
    }
  }

  TEST_CASE("ComponentTooltipController copies only popover shell classes", "[gtk][unit][layout][component]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.tooltip_controller_test");

    auto target = Gtk::Button{};
    auto tooltipBox = Gtk::Box{};
    tooltipBox.add_css_class("ao-popover-transparent");
    tooltipBox.add_css_class("ao-opacity-80");

    auto tooltipComponent = StaticWidgetComponent{tooltipBox};
    auto controller = ComponentTooltipController{};
    controller.attach(target, tooltipComponent);

    auto* const popover = findPopoverChild(target);
    REQUIRE(popover != nullptr);
    CHECK(popover->has_css_class("ao-popover-transparent"));
    CHECK_FALSE(popover->has_css_class("ao-opacity-80"));
  }
} // namespace ao::gtk::layout::test
