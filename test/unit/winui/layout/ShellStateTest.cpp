// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/ShellState.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>

namespace ao::winui::test
{
  namespace
  {
    /// The state a width resolves to while the user has requested neither reveal nor dismissal.
    ShellState unrequested(ShellMode const mode, double const width) noexcept
    {
      return resolveShellState(mode, width, std::nullopt);
    }
  } // namespace

  TEST_CASE("ShellState - responsive tiers preserve the track and transport surfaces", "[winui][unit][layout]")
  {
    auto const wide = unrequested(ShellMode::Modern, kWideShellWidth);
    CHECK(wide.widthClass == ShellWidthClass::Wide);
    CHECK(wide.navigation == NavigationPaneMode::Expanded);
    CHECK(wide.inspector == InspectorPaneMode::Inline);
    CHECK(wide.integratedTitleBar);

    auto const medium = unrequested(ShellMode::Modern, kMediumShellWidth);
    CHECK(medium.widthClass == ShellWidthClass::Medium);
    CHECK(medium.navigation == NavigationPaneMode::Compact);
    CHECK(medium.inspector == InspectorPaneMode::Overlay);

    auto const narrow = unrequested(ShellMode::Modern, 480.0);
    CHECK(narrow.widthClass == ShellWidthClass::Narrow);
    CHECK(narrow.navigation == NavigationPaneMode::Overlay);
    CHECK(narrow.inspector == InspectorPaneMode::Overlay);

    // Pin the published pixel thresholds independently of the constants used by the resolver.
    CHECK(kMediumShellWidth == 720.0);
    CHECK(kWideShellWidth == 1120.0);

    for (auto const mode : {ShellMode::Modern, ShellMode::Classic})
    {
      for (auto const width : {719.0, 719.9})
      {
        INFO("mode " << static_cast<int>(mode) << ", width " << width);
        auto const state = unrequested(mode, width);
        CHECK(state.widthClass == ShellWidthClass::Narrow);
        CHECK(state.navigation == NavigationPaneMode::Overlay);
        CHECK(state.inspector == InspectorPaneMode::Overlay);
        CHECK_FALSE(state.inspectorRevealed);
      }

      for (auto const width : {720.0, 720.1, 1119.9})
      {
        INFO("mode " << static_cast<int>(mode) << ", width " << width);
        auto const state = unrequested(mode, width);
        CHECK(state.widthClass == ShellWidthClass::Medium);
        CHECK(state.navigation == NavigationPaneMode::Compact);
        CHECK(state.inspector == InspectorPaneMode::Overlay);
        CHECK_FALSE(state.inspectorRevealed);
      }

      for (auto const width : {1120.0, 1120.1})
      {
        INFO("mode " << static_cast<int>(mode) << ", width " << width);
        auto const state = unrequested(mode, width);
        CHECK(state.widthClass == ShellWidthClass::Wide);
        CHECK(state.navigation == NavigationPaneMode::Expanded);
        CHECK(state.inspector == InspectorPaneMode::Inline);
        CHECK(state.inspectorRevealed);
      }
    }
  }

  TEST_CASE("ShellState - mode changes chrome without changing responsive visibility", "[winui][unit][layout]")
  {
    auto const modern = unrequested(ShellMode::Modern, 900.0);
    auto const classic = unrequested(ShellMode::Classic, 900.0);

    CHECK(modern.widthClass == classic.widthClass);
    CHECK(modern.navigation == classic.navigation);
    CHECK(modern.inspector == classic.inspector);
    CHECK(modern.integratedTitleBar);
    CHECK_FALSE(classic.integratedTitleBar);
  }

  TEST_CASE("ShellState - an unasked inspector shows exactly where it costs the workspace nothing",
            "[winui][unit][layout]")
  {
    // Inline, the inspector owns a workspace column and is visible by default.
    // Below the wide tier it overlays the workspace and waits to be requested.
    CHECK(unrequested(ShellMode::Modern, kWideShellWidth).inspectorRevealed);

    for (auto const width : {480.0, kMediumShellWidth, kWideShellWidth - 1.0})
    {
      INFO("width " << width);
      CHECK(unrequested(ShellMode::Modern, width).inspector == InspectorPaneMode::Overlay);
      CHECK_FALSE(unrequested(ShellMode::Modern, width).inspectorRevealed);
      CHECK_FALSE(unrequested(ShellMode::Classic, width).inspectorRevealed);
    }
  }

  TEST_CASE("ShellState - an asked inspector answers the request at every width", "[winui][unit][layout]")
  {
    // The request survives responsive transitions: explicit dismissal stays
    // dismissed when inline, and an explicit reveal stays revealed when wide.
    for (auto const width : {480.0, kMediumShellWidth, kWideShellWidth})
    {
      INFO("width " << width);

      for (auto const mode : {ShellMode::Modern, ShellMode::Classic})
      {
        CHECK(resolveShellState(mode, width, true).inspectorRevealed);
        CHECK_FALSE(resolveShellState(mode, width, false).inspectorRevealed);
      }
    }
  }
} // namespace ao::winui::test
