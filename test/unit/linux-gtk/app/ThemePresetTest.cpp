// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ThemePreset.h"

#include <ao/rt/AppPrefsState.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::test
{
  TEST_CASE("ThemePreset - maps GTK CSS classes for each preset", "[gtk][unit][app][theme]")
  {
    CHECK(themeCssClass(rt::ThemePresetId::Classic) == "ao-theme-classic");
    CHECK(themeCssClass(rt::ThemePresetId::Modern) == "ao-theme-modern");
  }
} // namespace ao::gtk::test
