// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AobusSoulWindow.h"

#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include <ao/rt/AppRuntime.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::test
{
  // Shell smoke only. View-state policy belongs to AobusSoulViewModelTest;
  // this case does not exercise click/Escape dispatch or queued callback retirement.
  TEST_CASE("AobusSoulWindow - constructs and hides the visualizer shell", "[gtk][unit][playback][soul]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = AobusSoulWindow{};

    CHECK(window.get_title() == "Aobus Soul");
    CHECK(window.get_name() == "AobusSoul");
    CHECK(hasCssClass(window, "ao-soul-window"));

    window.bind(fixture.runtime().playback());
    REQUIRE_FALSE(window.get_visible());
    window.show();
    drainGtkEvents();
    REQUIRE(window.get_visible());
    window.hide();
    drainGtkEvents();
    CHECK_FALSE(window.get_visible());
  }
} // namespace ao::gtk::test
