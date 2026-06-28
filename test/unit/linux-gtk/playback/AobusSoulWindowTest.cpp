// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AobusSoulWindow.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::test
{
  // Smoke only. The soul view state (breathe/aura) is covered by
  // AobusSoulViewModelTest; the click/Escape -> hide wiring is trivial GTK glue.
  TEST_CASE("AobusSoulWindow constructs and hides the visualizer shell", "[gtk][unit][smoke]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();

    auto window = AobusSoulWindow{};

    CHECK(window.get_title() == "Aobus Soul");
    CHECK(window.get_name() == "AobusSoul");
    CHECK(hasCssClass(window, "ao-soul-window"));

    window.bind(playback);
    window.show();
    drainGtkEvents();
    window.hide();
    drainGtkEvents();
  }
} // namespace ao::gtk::test
