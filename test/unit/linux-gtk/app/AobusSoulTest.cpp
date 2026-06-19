// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/AobusSoul.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gdkmm/rgba.h>
#include <gtkmm/enums.h>
#include <gtkmm/snapshot.h>

#include <cstdint>

namespace ao::gtk::test
{
  TEST_CASE("AobusSoul - basic functionality", "[gtk][app]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto soul = AobusSoul{};

    SECTION("initial state")
    {
      CHECK(soul.get_visible() == true);
    }

    SECTION("breathe toggles animation")
    {
      soul.breathe(true);
      soul.breathe(false);
    }

    SECTION("setAura updates color")
    {
      auto color = Gdk::RGBA{"#ff0000"};
      soul.setAura(color);
    }

    SECTION("setShowFullLogo updates state")
    {
      soul.setShowFullLogo(true);
      soul.setShowFullLogo(false);
    }

    SECTION("Gtk::Widget vfuncs")
    {
      std::int32_t min = -1;
      std::int32_t nat = -1;
      std::int32_t minB = -1;
      std::int32_t natB = -1;
      soul.measure(Gtk::Orientation::HORIZONTAL, 100, min, nat, minB, natB);
      CHECK(min >= 0);
      CHECK(nat >= 0);

      CHECK(soul.get_request_mode() == Gtk::SizeRequestMode::CONSTANT_SIZE);
    }
  }
} // namespace ao::gtk::test
