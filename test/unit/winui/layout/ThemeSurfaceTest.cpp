// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/ThemeSurface.h>

#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/winui/Theme.h>
#include <ao/winui/layout/ElementKind.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <utility>

namespace ao::winui::test
{
  namespace
  {
    constexpr auto kAllSurfaces = std::to_array({
      ThemeSurface::Window,
      ThemeSurface::Surface,
      ThemeSurface::ModernNavigation,
      ThemeSurface::ModernInspector,
      ThemeSurface::ModernNowPlaying,
      ThemeSurface::ClassicToolbar,
      ThemeSurface::ClassicTree,
      ThemeSurface::ClassicStatusBar,
    });

    uimodel::LayoutNode surfacedNode(uimodel::LayoutValue surface)
    {
      return uimodel::LayoutNode{.type = "box", .layout = {{"surface", std::move(surface)}}};
    }
  } // namespace

  TEST_CASE("themeSurfaceFromString - every slot round-trips through its authored spelling", "[winui][unit][layout]")
  {
    for (auto const surface : kAllSurfaces)
    {
      auto const optParsed = themeSurfaceFromString(toString(surface));
      REQUIRE(optParsed);
      CHECK(*optParsed == surface);
    }
  }

  TEST_CASE("themeSurfaceFromString - a slot the shell does not paint is not a surface", "[winui][unit][layout]")
  {
    CHECK_FALSE(themeSurfaceFromString("modern.nowhere").has_value());
    CHECK_FALSE(themeSurfaceFromString("").has_value());
    CHECK_FALSE(themeSurfaceFromString("Window").has_value());
  }

  TEST_CASE("themeSurfaceToken - each slot reads the token its own theme section carries", "[winui][unit][layout]")
  {
    auto theme = Theme{};
    theme.shared.windowBackground = "#010101";
    theme.shared.surface = "#020202";
    theme.modern.navigationBackground = "#030303";
    theme.modern.inspectorBackground = "#040404";
    theme.modern.nowPlayingBackground = "#050505";
    theme.classic.toolbarBackground = "#060606";
    theme.classic.treeBackground = "#070707";
    theme.classic.statusBackground = "#080808";

    CHECK(themeSurfaceToken(theme, ThemeSurface::Window) == "#010101");
    CHECK(themeSurfaceToken(theme, ThemeSurface::Surface) == "#020202");
    CHECK(themeSurfaceToken(theme, ThemeSurface::ModernNavigation) == "#030303");
    CHECK(themeSurfaceToken(theme, ThemeSurface::ModernInspector) == "#040404");
    CHECK(themeSurfaceToken(theme, ThemeSurface::ModernNowPlaying) == "#050505");
    CHECK(themeSurfaceToken(theme, ThemeSurface::ClassicToolbar) == "#060606");
    CHECK(themeSurfaceToken(theme, ThemeSurface::ClassicTree) == "#070707");
    CHECK(themeSurfaceToken(theme, ThemeSurface::ClassicStatusBar) == "#080808");
  }

  TEST_CASE("themeSurfaceToken - every slot names a token the theme actually carries", "[winui][unit][layout]")
  {
    auto const theme = Theme{};

    for (auto const surface : kAllSurfaces)
    {
      CHECK_FALSE(themeSurfaceToken(theme, surface).empty());
    }
  }

  TEST_CASE("elementKindAcceptsSurface - only the kinds that own a background accept one", "[winui][unit][layout]")
  {
    CHECK(elementKindAcceptsSurface(ElementKind::Grid));
    CHECK(elementKindAcceptsSurface(ElementKind::Border));
    CHECK(elementKindAcceptsSurface(ElementKind::NavigationView));
    CHECK(elementKindAcceptsSurface(ElementKind::TreeView));
    CHECK(elementKindAcceptsSurface(ElementKind::ListView));
    CHECK(elementKindAcceptsSurface(ElementKind::ScrollViewer));

    // A TextBlock draws glyphs and nothing behind them, and a bare
    // FrameworkElement draws nothing at all.
    CHECK_FALSE(elementKindAcceptsSurface(ElementKind::TextBlock));
    CHECK_FALSE(elementKindAcceptsSurface(ElementKind::FrameworkElement));
  }

  TEST_CASE("planThemeSurface - an authored slot plans the surface it names", "[winui][unit][layout]")
  {
    auto const optSurface = planThemeSurface(surfacedNode(uimodel::LayoutValue{std::string{"classic.toolbar"}}));

    REQUIRE(optSurface);
    CHECK(*optSurface == ThemeSurface::ClassicToolbar);
  }

  TEST_CASE("planThemeSurface - a node without a usable slot plans nothing", "[winui][unit][layout]")
  {
    CHECK_FALSE(planThemeSurface(uimodel::LayoutNode{.type = "box"}).has_value());
    CHECK_FALSE(planThemeSurface(surfacedNode(uimodel::LayoutValue{std::string{}})).has_value());
    CHECK_FALSE(planThemeSurface(surfacedNode(uimodel::LayoutValue{std::string{"modern.nowhere"}})).has_value());
  }
} // namespace ao::winui::test
