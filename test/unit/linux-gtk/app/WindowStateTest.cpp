// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "app/WindowState.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::test
{
  TEST_CASE("recordWindowGeometry - maximized checkpoint retains current-session normal geometry",
            "[gtk][regression][app][geometry]")
  {
    auto state = WindowState{.width = 720, .height = 540, .maximized = false};

    recordWindowGeometry(state, WindowState{.width = 900, .height = 700, .maximized = false});
    // A maximized window reports its maximized extent, which is not the size to
    // restore on the next unmaximized start.
    recordWindowGeometry(state, WindowState{.width = 1280, .height = 1024, .maximized = true});

    CHECK(state.width == 900);
    CHECK(state.height == 700);
    CHECK(state.maximized);
  }

  TEST_CASE("recordWindowGeometry - unrealized extents do not replace a chosen size", "[gtk][unit][app][geometry]")
  {
    auto state = WindowState{.width = 900, .height = 700, .maximized = true};

    recordWindowGeometry(state, WindowState{.width = 0, .height = 0, .maximized = false});

    CHECK(state.width == 900);
    CHECK(state.height == 700);
    CHECK_FALSE(state.maximized);
  }
} // namespace ao::gtk::test
