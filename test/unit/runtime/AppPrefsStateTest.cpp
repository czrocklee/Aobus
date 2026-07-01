// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/AppPrefsState.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::rt::test
{
  TEST_CASE("AppPrefsState - theme preset ids round trip with classic fallback", "[runtime][unit][app-prefs]")
  {
    CHECK(themePresetToString(ThemePresetId::Classic) == "classic");
    CHECK(themePresetToString(ThemePresetId::Modern) == "modern");

    CHECK(themePresetFromString("classic") == ThemePresetId::Classic);
    CHECK(themePresetFromString("modern") == ThemePresetId::Modern);
    CHECK(themePresetFromString("unknown") == ThemePresetId::Classic);
    CHECK(themePresetFromString("") == ThemePresetId::Classic);
  }
} // namespace ao::rt::test
