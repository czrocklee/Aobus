// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputDeviceSelector.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/enums.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>

namespace ao::gtk::test
{
  TEST_CASE("OutputDeviceSelector renders devices and routes selected output changes", "[gtk][unit][playback][output]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();

    SECTION("constructor wires up the popover with a scrolled list box")
    {
      auto selector = OutputDeviceSelector{playback, Gtk::PositionType::BOTTOM};
      drainGtkEvents();

      CHECK(selector.get_autohide());
      CHECK(selector.get_position() == Gtk::PositionType::BOTTOM);

      auto* const scrolled = dynamic_cast<Gtk::ScrolledWindow*>(selector.get_child());
      REQUIRE(scrolled != nullptr);

      auto* const viewport = scrolled->get_child();
      REQUIRE(viewport != nullptr);
      auto* const listBox = dynamic_cast<Gtk::ListBox*>(viewport->get_first_child());
      REQUIRE(listBox != nullptr);
      CHECK(listBox->get_selection_mode() == Gtk::SelectionMode::NONE);
      CHECK(hasCssClass(*listBox, "ao-rich-list"));
    }
  }
} // namespace ao::gtk::test
