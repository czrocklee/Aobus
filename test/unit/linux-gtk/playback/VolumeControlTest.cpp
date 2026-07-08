// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/VolumeControl.h"

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/button.h>
#include <gtkmm/image.h>
#include <gtkmm/window.h>

namespace ao::gtk::test
{
  TEST_CASE("VolumeControl - renders model state into the button", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};

    auto& playback = fixture.runtime().playback();
    rt::test::addReadyAudioProvider(playback);
    drainGtkEvents();
    playback.setVolume(0.5F);

    auto control = VolumeControl{playback};
    auto* btn = dynamic_cast<Gtk::Button*>(&control.widget());
    REQUIRE(btn != nullptr);
    auto* icon = dynamic_cast<Gtk::Image*>(btn->get_child());
    REQUIRE(icon != nullptr);

    auto window = Gtk::Window{};
    window.set_child(*btn);
    window.set_default_size(100, 20);

    drainGtkEvents();

    CHECK(btn->get_visible());
    CHECK_FALSE(btn->get_tooltip_text().empty());
    CHECK_FALSE(icon->get_icon_name().empty());
  }
} // namespace ao::gtk::test
