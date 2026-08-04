// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "theme/SurfaceBrushes.h"

#include "layout/runtime/CommonLayoutProps.h"
#include "pch.h"
#include <ao/winui/Theme.h>
#include <ao/winui/layout/ThemeSurface.h>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.UI.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::winui
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::Media::Brush;
    using winrt::Microsoft::UI::Xaml::Media::SolidColorBrush;

    constexpr auto kArgbTokenLength = std::size_t{9};
    constexpr auto kOpaqueAlpha = std::uint8_t{0xFF};

    constexpr auto kCardBackgroundKey = std::wstring_view{L"CardBackgroundFillColorDefaultBrush"};
    constexpr auto kPageBackgroundKey = std::wstring_view{L"ApplicationPageBackgroundThemeBrush"};

    winrt::Windows::UI::Color parseColor(std::string_view const token)
    {
      auto const offset = token.size() == kArgbTokenLength ? 3U : 1U;
      auto const component = [token](std::size_t const at)
      { return static_cast<std::uint8_t>(std::stoul(std::string{token.substr(at, 2)}, nullptr, 16)); };
      return {
        .A = token.size() == kArgbTokenLength ? component(1) : kOpaqueAlpha,
        .R = component(offset),
        .G = component(offset + 2),
        .B = component(offset + 4),
      };
    }

    /// A stock brush the frame ships with, or nothing when the key is absent.
    Brush applicationBrush(std::wstring_view const key)
    {
      auto const application = winrt::Microsoft::UI::Xaml::Application::Current();

      if (!application || !application.Resources())
      {
        return nullptr;
      }

      auto const boxed = winrt::box_value(key);
      auto const resources = application.Resources();
      return resources.HasKey(boxed) ? resources.Lookup(boxed).try_as<Brush>() : nullptr;
    }

    /**
     * @brief What each slot looks like with no theme override loaded.
     *
     * These are the same brushes the frame applies when a theme file is removed:
     * the window, the shared surface, and the navigation pane keep the stock
     * chrome, so they name no brush at all.
     */
    Brush systemBrush(ThemeSurface const surface)
    {
      switch (surface)
      {
        case ThemeSurface::Window:
        case ThemeSurface::Surface:
        case ThemeSurface::ModernNavigation: return nullptr;
        case ThemeSurface::ModernInspector:
        case ThemeSurface::ModernNowPlaying:
        case ThemeSurface::ClassicToolbar:
        case ThemeSurface::ClassicStatusBar: return applicationBrush(kCardBackgroundKey);
        case ThemeSurface::ClassicTree: return applicationBrush(kPageBackgroundKey);
      }

      return nullptr;
    }
  } // namespace

  SolidColorBrush themeColorBrush(std::string_view const token)
  {
    return SolidColorBrush{parseColor(token)};
  }

  layout::SurfaceBrushResolver makeSurfaceBrushResolver(std::optional<Theme> optTheme)
  {
    return [optTheme = std::move(optTheme)](ThemeSurface const surface) -> Brush
    {
      if (!optTheme)
      {
        return systemBrush(surface);
      }

      return themeColorBrush(themeSurfaceToken(*optTheme, surface));
    };
  }
} // namespace ao::winui
