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
  TEST_CASE("AobusSoul renders widget state and applies presentation setters", "[gtk][unit][app][soul]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto soul = AobusSoul{};

    SECTION("initial widget state")
    {
      CHECK(soul.get_visible() == true);
      CHECK(soul.has_css_class("ao-soul"));
      CHECK_FALSE(soul.isBreathing());
      CHECK_FALSE(soul.showFullLogo());
    }

    SECTION("breathe toggles animation state")
    {
      soul.breathe(true);
      CHECK(soul.isBreathing());

      soul.breathe(false);
      CHECK_FALSE(soul.isBreathing());
    }

    SECTION("setAura updates color")
    {
      auto color = Gdk::RGBA{"#ff0000"};
      soul.setAura(color);
      CHECK(soul.aura() == color);
    }

    SECTION("setShowFullLogo updates render state")
    {
      soul.setShowFullLogo(true);
      CHECK(soul.showFullLogo());

      soul.setShowFullLogo(false);
      CHECK_FALSE(soul.showFullLogo());
    }

    SECTION("Gtk::Widget sizing contract")
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
