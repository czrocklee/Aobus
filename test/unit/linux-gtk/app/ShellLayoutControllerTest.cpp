// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ShellLayoutController.h"

#include "app/AppConfig.h"
#include "app/ThemeCoordinator.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/applicationwindow.h>

#include <memory>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("ShellLayoutController - lifecycle", "[gtk][app][shell]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto window = Gtk::ApplicationWindow{};
    window.set_application(appPtr);

    auto themeController = ThemeCoordinator{};
    auto controller = ShellLayoutController{runtime, window, nullptr, themeController};

    SECTION("initial state")
    {
      // Registry should have standard components
      // We don't have public way to check count without peering
    }

    SECTION("attachToWindow sets child")
    {
      controller.attachToWindow();
      CHECK(window.get_child() != nullptr);
    }

    SECTION("loadLayout doesn't crash")
    {
      // Allocate dynamically and keep alive indefinitely to prevent ASAN stack-use-after-scope
      // when the async worker executes the loadLayout coroutine, and to prevent ASAN memory leak detection.
      static std::vector<std::unique_ptr<AppConfig>> keepAlive;
      keepAlive.push_back(std::make_unique<AppConfig>(fixture.tempDir().path()));

      controller.loadLayout(*keepAlive.back());
      drainGtkEvents();
    }

    SECTION("attachToWindow exports actions and refreshExportedActions works")
    {
      controller.attachToWindow();

      auto* actionMap = dynamic_cast<Gio::ActionMap*>(&window);
      REQUIRE(actionMap != nullptr);

      auto gioActionPtr = actionMap->lookup_action("playback.stop");
      REQUIRE(gioActionPtr != nullptr);

      // Queue model is not bound, so hasActiveQueue is false, thus stop should be disabled
      controller.refreshExportedActions();
      CHECK(gioActionPtr->property_enabled() == false);
    }
  }
} // namespace ao::gtk::test
