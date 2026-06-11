// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ShellLayoutController.h"

#include "app/AppConfig.h"
#include "app/ShellLayoutStore.h"
#include "app/ThemeCoordinator.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/applicationwindow.h>

#include <memory>

namespace ao::gtk::test
{
  TEST_CASE("ShellLayoutController - lifecycle", "[gtk][app][shell]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto window = Gtk::ApplicationWindow{};
    window.set_application(appPtr);

    auto const tempDir = fixture.tempDir().path();
    auto const configPtr = std::make_shared<AppConfig>(tempDir / "config.yaml");
    auto const storePtr = std::make_shared<ShellLayoutStore>(tempDir / "layouts");
    auto themeController = ThemeCoordinator{};
    auto controller = ShellLayoutController{runtime, window, configPtr, storePtr, themeController};

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

    SECTION("loadLayout load works")
    {
      controller.loadLayout(*configPtr);
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
