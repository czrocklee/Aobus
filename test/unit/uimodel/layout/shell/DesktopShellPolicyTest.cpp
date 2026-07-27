// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/shell/DesktopShellPolicy.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("DesktopShellPolicy - responsive tiers preserve the track and transport surfaces",
            "[uimodel][unit][layout][shell]")
  {
    auto const wide = DesktopShellPolicy::resolve(DesktopShellMode::Modern, DesktopShellPolicy::kWideWidth);
    CHECK(wide.widthClass == DesktopShellWidthClass::Wide);
    CHECK(wide.navigation == DesktopNavigationPresentation::Expanded);
    CHECK(wide.inspector == DesktopInspectorPresentation::Inline);
    CHECK(wide.integratedTitleBar);

    auto const medium = DesktopShellPolicy::resolve(DesktopShellMode::Modern, DesktopShellPolicy::kMediumWidth);
    CHECK(medium.widthClass == DesktopShellWidthClass::Medium);
    CHECK(medium.navigation == DesktopNavigationPresentation::Compact);
    CHECK(medium.inspector == DesktopInspectorPresentation::Overlay);

    auto const narrow = DesktopShellPolicy::resolve(DesktopShellMode::Modern, 480.0);
    CHECK(narrow.widthClass == DesktopShellWidthClass::Narrow);
    CHECK(narrow.navigation == DesktopNavigationPresentation::Overlay);
    CHECK(narrow.inspector == DesktopInspectorPresentation::Overlay);
  }

  TEST_CASE("DesktopShellPolicy - mode changes chrome without changing responsive visibility",
            "[uimodel][unit][layout][shell]")
  {
    auto const modern = DesktopShellPolicy::resolve(DesktopShellMode::Modern, 900.0);
    auto const classic = DesktopShellPolicy::resolve(DesktopShellMode::Classic, 900.0);

    CHECK(modern.widthClass == classic.widthClass);
    CHECK(modern.navigation == classic.navigation);
    CHECK(modern.inspector == classic.inspector);
    CHECK(modern.integratedTitleBar);
    CHECK_FALSE(classic.integratedTitleBar);
  }
} // namespace ao::uimodel::test
