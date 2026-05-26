// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/VolumeBar.h"
#include "playback/VolumeControl.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/window.h>

using namespace ao;
using namespace ao::gtk;
using namespace ao::gtk::test;

TEST_CASE("VolumeControl - GTK smoke test", "[gtk][playback][viewmodel]")
{
  [[maybe_unused]] auto const app = ensureGtkApplication();
  auto fixture = GtkRuntimeFixture{};

  auto& playback = fixture.runtime().playback();

  auto control = VolumeControl{playback};
  auto& bar = dynamic_cast<VolumeBar&>(control.widget());

  auto window = Gtk::Window{};
  window.set_child(bar);
  window.set_default_size(100, 20);

  drainGtkEvents();
}
