// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "../../GtkTestSupport.h"
#include "app/linux-gtk/image/ImageCache.h"
#include "app/linux-gtk/layout/document/LayoutNode.h"
#include "app/linux-gtk/layout/runtime/ActionRegistry.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "test/unit/lmdb/TestUtils.h"


#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/button.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <memory>

namespace ao::gtk::layout::test
{
  using namespace ao::lmdb::test;
  using ao::gtk::test::makeRuntime;

  namespace
  {
  } // namespace

  TEST_CASE("playback.image variant support", "[layout][unit][components]")
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

    SECTION("default variant has no extra styling")
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

    SECTION("thumbnail variant applies size and CSS class")
    {
      auto node = LayoutNode{.type = "playback.image"};
      node.props["variant"] = LayoutValue{"thumbnail"};
      auto const compPtr = registry.create(ctx, node);

      REQUIRE(compPtr != nullptr);
      auto& widget = compPtr->widget();

      auto* const button = dynamic_cast<Gtk::Button*>(&widget);
      REQUIRE(button != nullptr);
      auto* const picture = button->get_child();
      REQUIRE(picture != nullptr);

      CHECK(picture->has_css_class("ao-nowplaying-image-thumb"));
      CHECK_FALSE(widget.get_hexpand());
      CHECK_FALSE(widget.get_vexpand());

      std::int32_t width = -1;
      std::int32_t height = -1;
      picture->get_size_request(width, height);
      CHECK(width == 56);
      CHECK(height == 56);
    }
  }
} // namespace ao::gtk::layout::test
