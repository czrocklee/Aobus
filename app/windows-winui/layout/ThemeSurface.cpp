// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/winui/layout/ThemeSurface.h>

#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/winui/Theme.h>
#include <ao/winui/layout/ElementKind.h>

#include <optional>
#include <string>
#include <string_view>

namespace ao::winui
{
  std::string_view toString(ThemeSurface const surface) noexcept
  {
    switch (surface)
    {
      case ThemeSurface::Window: return "window";
      case ThemeSurface::Surface: return "surface";
      case ThemeSurface::ModernNavigation: return "modern.navigation";
      case ThemeSurface::ModernInspector: return "modern.inspector";
      case ThemeSurface::ModernNowPlaying: return "modern.nowPlaying";
      case ThemeSurface::ClassicToolbar: return "classic.toolbar";
      case ThemeSurface::ClassicTree: return "classic.tree";
      case ThemeSurface::ClassicStatusBar: return "classic.statusBar";
    }

    return "window";
  }

  std::optional<ThemeSurface> themeSurfaceFromString(std::string_view const name) noexcept
  {
    if (name == "window")
    {
      return ThemeSurface::Window;
    }

    if (name == "surface")
    {
      return ThemeSurface::Surface;
    }

    if (name == "modern.navigation")
    {
      return ThemeSurface::ModernNavigation;
    }

    if (name == "modern.inspector")
    {
      return ThemeSurface::ModernInspector;
    }

    if (name == "modern.nowPlaying")
    {
      return ThemeSurface::ModernNowPlaying;
    }

    if (name == "classic.toolbar")
    {
      return ThemeSurface::ClassicToolbar;
    }

    if (name == "classic.tree")
    {
      return ThemeSurface::ClassicTree;
    }

    if (name == "classic.statusBar")
    {
      return ThemeSurface::ClassicStatusBar;
    }

    return std::nullopt;
  }

  std::string_view themeSurfaceToken(Theme const& theme, ThemeSurface const surface) noexcept
  {
    switch (surface)
    {
      case ThemeSurface::Window: return theme.shared.windowBackground;
      case ThemeSurface::Surface: return theme.shared.surface;
      case ThemeSurface::ModernNavigation: return theme.modern.navigationBackground;
      case ThemeSurface::ModernInspector: return theme.modern.inspectorBackground;
      case ThemeSurface::ModernNowPlaying: return theme.modern.nowPlayingBackground;
      case ThemeSurface::ClassicToolbar: return theme.classic.toolbarBackground;
      case ThemeSurface::ClassicTree: return theme.classic.treeBackground;
      case ThemeSurface::ClassicStatusBar: return theme.classic.statusBackground;
    }

    return theme.shared.windowBackground;
  }

  bool elementKindAcceptsSurface(ElementKind const kind) noexcept
  {
    // The three XAML types that own a Background between them; everything the
    // presets construct either derives from one of these or paints nothing.
    return isElementKindDerivedFrom(kind, ElementKind::Panel) || isElementKindDerivedFrom(kind, ElementKind::Border) ||
           isElementKindDerivedFrom(kind, ElementKind::Control);
  }

  std::optional<ThemeSurface> planThemeSurface(uimodel::LayoutNode const& node)
  {
    auto const it = node.layout.find(kSurfaceLayoutProp);

    if (it == node.layout.end())
    {
      return std::nullopt;
    }

    auto const* const name = it->second.getIf<std::string>();

    if (name == nullptr)
    {
      return std::nullopt;
    }

    return themeSurfaceFromString(*name);
  }
} // namespace ao::winui
