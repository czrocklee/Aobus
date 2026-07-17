// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/preference/ThemePreset.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("ThemePreset - stable ids resolve with classic fallback", "[uimodel][unit][preferences]")
  {
    CHECK(themePresetId(ThemePreset::Classic) == "classic");
    CHECK(themePresetId(ThemePreset::Modern) == "modern");

    CHECK(themePresetFromId("classic") == ThemePreset::Classic);
    CHECK(themePresetFromId("modern") == ThemePreset::Modern);
    CHECK(themePresetFromId("unknown") == ThemePreset::Classic);
    CHECK(themePresetFromId("") == ThemePreset::Classic);
  }
} // namespace ao::uimodel::test
