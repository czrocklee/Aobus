// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/DismissController.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <array>

namespace ao::gtk::test
{
  // isWidgetWithinAny is the pure decision behind DismissController: it walks the widget-tree
  // ancestry, no geometry. The geometric hit-test (Gtk::Window::pick) lives at the call site and is
  // intentionally not exercised here.
  TEST_CASE("isWidgetWithinAny - ancestry membership", "[gtk][common][dismiss]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    // Declared leaf-first so containers (declared later) are destroyed first and unparent cleanly.
    auto leaf = Gtk::Label{};
    auto innerBox = Gtk::Box{};
    auto outerBox = Gtk::Box{};
    auto outside = Gtk::Label{};

    outerBox.append(innerBox);
    innerBox.append(leaf);

    SECTION("the target itself counts as inside")
    {
      auto const inside = std::array<Gtk::Widget*, 1>{&leaf};
      CHECK(isWidgetWithinAny(&leaf, inside));
    }

    SECTION("a direct ancestor counts as inside")
    {
      auto const inside = std::array<Gtk::Widget*, 1>{&innerBox};
      CHECK(isWidgetWithinAny(&leaf, inside));
    }

    SECTION("a transitive ancestor counts as inside")
    {
      auto const inside = std::array<Gtk::Widget*, 1>{&outerBox};
      CHECK(isWidgetWithinAny(&leaf, inside));
    }

    SECTION("a descendant of the target does not count")
    {
      auto const inside = std::array<Gtk::Widget*, 1>{&innerBox};
      CHECK_FALSE(isWidgetWithinAny(&outerBox, inside));
    }

    SECTION("an unrelated widget is outside")
    {
      auto const inside = std::array<Gtk::Widget*, 1>{&outerBox};
      CHECK_FALSE(isWidgetWithinAny(&outside, inside));
    }

    SECTION("a null target is outside")
    {
      auto const inside = std::array<Gtk::Widget*, 1>{&outerBox};
      CHECK_FALSE(isWidgetWithinAny(nullptr, inside));
    }

    SECTION("an empty inside list is always outside")
    {
      CHECK_FALSE(isWidgetWithinAny(&leaf, {}));
    }
  }
} // namespace ao::gtk::test
