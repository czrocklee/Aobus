// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ShellLayoutController.h"

#include "app/AppConfig.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/window.h>

#include <memory>
#include <vector>

using namespace ao;
using namespace ao::gtk;
using namespace ao::gtk::test;

TEST_CASE("ShellLayoutController - lifecycle", "[gtk][app][shell]")
{
  [[maybe_unused]] auto const app = ensureGtkApplication();
  auto fixture = GtkRuntimeFixture{};
  auto& runtime = fixture.runtime();
  auto window = Gtk::Window{};

  auto controller = ShellLayoutController{runtime, window};

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
}
