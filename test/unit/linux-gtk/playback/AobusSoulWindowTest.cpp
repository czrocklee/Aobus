// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AobusSoulWindow.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::test
{
  TEST_CASE("AobusSoulWindow - basic lifecycle", "[gtk][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();

    auto window = AobusSoulWindow{};

    SECTION("initial state")
    {
      CHECK(window.get_title() == "Aobus Soul");
    }

    SECTION("bind and show")
    {
      window.bind(playback);

      drainGtkEvents();
      window.hide();
      drainGtkEvents();
    }
  }
} // namespace ao::gtk::test
