// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/image/ImageCache.h"
#include "app/linux-gtk/layout/document/LayoutNode.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/layout/runtime/LayoutRuntime.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CorePrimitives.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/button.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace ao::gtk::layout::test
{
  using namespace ao::lmdb::test;

  namespace
  {
    class MockExecutor final : public rt::IControlExecutor
    {
    public:
      bool isCurrent() const noexcept override { return true; }
      void dispatch(std::move_only_function<void()> task) override { task(); }
      void defer(std::move_only_function<void()> task) override { task(); }
    };
  } // namespace

  TEST_CASE("playback.image variant support", "[layout][components]")
  {
    auto const app = Gtk::Application::create("io.github.aobus.playback_image_test");

    auto const tempDir = TempDir{};
    auto const configStore = std::make_shared<rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

    auto runtime = rt::AppRuntime{
      rt::AppRuntimeDependencies{.executor = std::make_unique<MockExecutor>(),
                                 .musicRoot = tempDir.path(),
                                 .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
                                 .workspaceConfigStore = configStore}};

    auto registry = ComponentRegistry{};
    LayoutRuntime::registerStandardComponents(registry);

    auto window = Gtk::Window{};
    auto imageCache = std::make_unique<ImageCache>(10);
    auto ctx = LayoutContext{
      .registry = registry, .runtime = runtime, .parentWindow = window, .inspector = {.imageCache = imageCache.get()}};

    SECTION("default variant has no extra styling")
    {
      auto const node = LayoutNode{.type = "playback.image"};
      auto const comp = registry.create(ctx, node);

      REQUIRE(comp != nullptr);
      auto& widget = comp->widget();

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
      auto const comp = registry.create(ctx, node);

      REQUIRE(comp != nullptr);
      auto& widget = comp->widget();

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
