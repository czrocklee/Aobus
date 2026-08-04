// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/ShellStatePolicy.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>

namespace ao::winui::test
{
  namespace
  {
    /// The state a width resolves to while the user has asked for nothing.
    ShellState unrequested(ShellMode const mode, double const width) noexcept
    {
      return ShellStatePolicy::resolve(mode, width, std::nullopt);
    }
  } // namespace

  TEST_CASE("ShellStatePolicy - responsive tiers preserve the track and transport surfaces", "[winui][unit][layout]")
  {
    auto const wide = unrequested(ShellMode::Modern, ShellStatePolicy::kWideWidth);
    CHECK(wide.widthClass == ShellWidthClass::Wide);
    CHECK(wide.navigation == NavigationPaneMode::Expanded);
    CHECK(wide.inspector == InspectorPaneMode::Inline);
    CHECK(wide.integratedTitleBar);

    auto const medium = unrequested(ShellMode::Modern, ShellStatePolicy::kMediumWidth);
    CHECK(medium.widthClass == ShellWidthClass::Medium);
    CHECK(medium.navigation == NavigationPaneMode::Compact);
    CHECK(medium.inspector == InspectorPaneMode::Overlay);

    auto const narrow = unrequested(ShellMode::Modern, 480.0);
    CHECK(narrow.widthClass == ShellWidthClass::Narrow);
    CHECK(narrow.navigation == NavigationPaneMode::Overlay);
    CHECK(narrow.inspector == InspectorPaneMode::Overlay);
  }

  TEST_CASE("ShellStatePolicy - mode changes chrome without changing responsive visibility", "[winui][unit][layout]")
  {
    auto const modern = unrequested(ShellMode::Modern, 900.0);
    auto const classic = unrequested(ShellMode::Classic, 900.0);

    CHECK(modern.widthClass == classic.widthClass);
    CHECK(modern.navigation == classic.navigation);
    CHECK(modern.inspector == classic.inspector);
    CHECK(modern.integratedTitleBar);
    CHECK_FALSE(classic.integratedTitleBar);
  }

  TEST_CASE("ShellStatePolicy - an unasked inspector shows exactly where it costs the workspace nothing",
            "[winui][unit][layout]")
  {
    // Inline the inspector is a column of the workspace and shows by default.
    // Every width below the wide tier makes it an overlay, which covers the
    // workspace and therefore waits to be asked for.
    CHECK(unrequested(ShellMode::Modern, ShellStatePolicy::kWideWidth).inspectorRevealed);

    for (auto const width : {480.0, ShellStatePolicy::kMediumWidth, ShellStatePolicy::kWideWidth - 1.0})
    {
      INFO("width " << width);
      CHECK(unrequested(ShellMode::Modern, width).inspector == InspectorPaneMode::Overlay);
      CHECK_FALSE(unrequested(ShellMode::Modern, width).inspectorRevealed);
      CHECK_FALSE(unrequested(ShellMode::Classic, width).inspectorRevealed);
    }
  }

  TEST_CASE("ShellStatePolicy - an asked inspector answers the request at every width", "[winui][unit][layout]")
  {
    /*
     * The request outlives the width that prompted it, and it decides both pane
     * modes: a user who dismissed the inspector inline does not want it back on
     * the next resize, and one who revealed the overlay narrow does not want it
     * gone once the window is wide enough to seat it inline.
     */
    for (auto const width : {480.0, ShellStatePolicy::kMediumWidth, ShellStatePolicy::kWideWidth})
    {
      INFO("width " << width);

      for (auto const mode : {ShellMode::Modern, ShellMode::Classic})
      {
        CHECK(ShellStatePolicy::resolve(mode, width, true).inspectorRevealed);
        CHECK_FALSE(ShellStatePolicy::resolve(mode, width, false).inspectorRevealed);
      }
    }
  }
} // namespace ao::winui::test
