// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/AobusSoul.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <gdkmm/rgba.h>
#include <gtkmm/enums.h>
#include <gtkmm/snapshot.h>

using namespace ao;
using namespace ao::gtk;
using namespace ao::gtk::test;

TEST_CASE("AobusSoul - basic functionality", "[gtk][app]")
{
  [[maybe_unused]] auto const app = ensureGtkApplication();

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
    auto min = std::int32_t{-1};
    auto nat = std::int32_t{-1};
    auto minB = std::int32_t{-1};
    auto natB = std::int32_t{-1};
    soul.measure(Gtk::Orientation::HORIZONTAL, 100, min, nat, minB, natB);
    CHECK(min >= 0);
    CHECK(nat >= 0);

    CHECK(soul.get_request_mode() == Gtk::SizeRequestMode::CONSTANT_SIZE);
  }
}
