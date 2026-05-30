// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "../../GtkTestSupport.h"
#include "app/linux-gtk/image/ImageCache.h"
#include "app/linux-gtk/layout/document/LayoutNode.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentTooltipController.h"
#include "app/linux-gtk/layout/runtime/ILayoutComponent.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "test/unit/lmdb/TestUtils.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/popover.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ao::gtk::layout::test
{
  using namespace ao::lmdb::test;
  using ao::gtk::test::makeRuntime;

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

  TEST_CASE("playback.image declarative properties", "[layout][unit][components]")
  {
    auto const appPtr = Gtk::Application::create("io.github.aobus.playback_image_test");

    auto const tempDir = TempDir{};
    auto runtime = makeRuntime(tempDir);

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto imageCachePtr = std::make_unique<ImageCache>(10);
    auto const actionRegistry = ActionRegistry{};
    auto ctx = LayoutContext{.registry = registry,
                             .actionRegistry = actionRegistry,
                             .runtime = runtime,
                             .parentWindow = window,
                             .inspector = {.imageCache = imageCachePtr.get()}};

    SECTION("default image has no extra styling")
    {
      auto const node = LayoutNode{.type = "playback.image"};
      auto const compPtr = registry.create(ctx, node);

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
      node.props["targetSize"] = LayoutValue{static_cast<std::int64_t>(56)};
      node.props["forceSquare"] = LayoutValue{true};
      node.props["opacity"] = LayoutValue{std::string{"0.5"}};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);
      auto& widget = compPtr->widget();

      auto* const button = dynamic_cast<Gtk::Button*>(&widget);
      REQUIRE(button != nullptr);
      auto* const picture = button->get_child();
      REQUIRE(picture != nullptr);

      CHECK(button->get_overflow() == Gtk::Overflow::HIDDEN);
      CHECK(picture->get_overflow() == Gtk::Overflow::HIDDEN);
      CHECK_FALSE(widget.get_hexpand());
      CHECK_FALSE(widget.get_vexpand());

      std::int32_t width = -1;
      std::int32_t height = -1;
      picture->get_size_request(width, height);
      CHECK(width == 56);
      CHECK(height == 56);
      CHECK(button->get_opacity() == Catch::Approx{0.5}.margin(0.01));
    }
  }

  TEST_CASE("ComponentTooltipController copies only popover shell classes", "[layout][unit][components]")
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
