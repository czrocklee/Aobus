// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/MouseNavigationPolicy.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::test
{
  TEST_CASE("mouseButtonNavigation maps thumb buttons to history navigation", "[gtk][unit][app][navigation]")
  {
    SECTION("button 8 navigates back")
    {
      auto const optNavigation = mouseButtonNavigation(8);
      REQUIRE(optNavigation.has_value());
      CHECK(*optNavigation == WorkspaceNavigation::Back);
    }

    SECTION("button 9 navigates forward")
    {
      auto const optNavigation = mouseButtonNavigation(9);
      REQUIRE(optNavigation.has_value());
      CHECK(*optNavigation == WorkspaceNavigation::Forward);
    }

    SECTION("primary, middle, and secondary buttons do not navigate")
    {
      CHECK_FALSE(mouseButtonNavigation(1).has_value());
      CHECK_FALSE(mouseButtonNavigation(2).has_value());
      CHECK_FALSE(mouseButtonNavigation(3).has_value());
    }

    SECTION("unknown buttons do not navigate")
    {
      CHECK_FALSE(mouseButtonNavigation(0).has_value());
      CHECK_FALSE(mouseButtonNavigation(10).has_value());
    }
  }
} // namespace ao::gtk::test
