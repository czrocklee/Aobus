// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/VolumeControl.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/button.h>
#include <gtkmm/window.h>

namespace ao::gtk::test
{
  TEST_CASE("VolumeControl - GTK smoke test", "[gtk][playback][viewmodel]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto& playback = fixture.runtime().playback();

    auto control = VolumeControl{playback};
    auto* btn = dynamic_cast<Gtk::Button*>(&control.widget());
    REQUIRE(btn != nullptr);

    auto window = Gtk::Window{};
    window.set_child(*btn);
    window.set_default_size(100, 20);

    drainGtkEvents();
  }
} // namespace ao::gtk::test
