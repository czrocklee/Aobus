// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "theme/SurfaceBrushes.h"

#include "layout/runtime/SurfaceBrushResolver.h"
#include <ao/winui/Theme.h>
#include <ao/winui/layout/ThemeSurface.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/base.h>

#include <array>
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
    using winrt::Microsoft::UI::Xaml::CornerRadius;
    using winrt::Microsoft::UI::Xaml::ResourceDictionary;
    using winrt::Microsoft::UI::Xaml::Controls::Grid;
    using winrt::Microsoft::UI::Xaml::Media::Brush;
    using winrt::Microsoft::UI::Xaml::Media::FontFamily;
    using winrt::Microsoft::UI::Xaml::Media::SolidColorBrush;
    using winrt::Windows::Foundation::IInspectable;

    constexpr std::size_t kArgbTokenLength = 9;
    constexpr std::uint8_t kOpaqueAlpha = 0xFF;
    constexpr double kClassicChromeCornerRadius = 4.0;

    constexpr auto kThemeResourceKeys = std::to_array<wchar_t const*>({
      L"ContentControlThemeFontFamily",
      L"BodyFontFamily",
      L"TextFillColorPrimaryBrush",
      L"TextFillColorSecondaryBrush",
      L"TextFillColorTertiaryBrush",
      L"DividerStrokeColorDefaultBrush",
      L"AccentFillColorDefaultBrush",
      L"AccentTextFillColorPrimaryBrush",
      L"ListViewItemBackgroundSelected",
      L"ListViewItemBackgroundSelectedPointerOver",
      L"ListViewItemBackgroundSelectedPressed",
      L"ListViewItemForegroundSelected",
    });

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

    SolidColorBrush themeColorBrush(std::string_view const token)
    {
      return SolidColorBrush{parseColor(token)};
    }

    void setResource(ResourceDictionary const& resources, wchar_t const* key, IInspectable const& value)
    {
      resources.Insert(winrt::box_value(key), value);
    }

    /**
     * @brief Replace the rounding the Classic chrome styles resolve.
     *
     * The key is declared in the window resources rather than removed and
     * restored: a `ThemeResource` that resolves to nothing throws when the
     * style is applied, and every rebuild applies these styles again.
     */
    void setClassicChromeCornerRadius(ResourceDictionary const& resources, double const radius)
    {
      resources.Insert(winrt::box_value(L"ClassicChromeCornerRadius"),
                       winrt::box_value(CornerRadius{
                         .TopLeft = radius, .TopRight = radius, .BottomRight = radius, .BottomLeft = radius}));
    }

    void clearThemeResources(ResourceDictionary const& resources)
    {
      for (auto const* key : kThemeResourceKeys)
      {
        resources.Remove(winrt::box_value(key));
      }
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

  void applyFrameTheme(Grid const& root, Theme const& theme)
  {
    auto const resources = root.Resources();
    clearThemeResources(resources);
    auto const primary = themeColorBrush(theme.shared.textPrimary);
    auto const secondary = themeColorBrush(theme.shared.textSecondary);
    auto const divider = themeColorBrush(theme.shared.divider);
    auto const accent = themeColorBrush(theme.shared.accent);
    auto const selection = themeColorBrush(theme.shared.selection);
    auto const font = FontFamily{winrt::to_hstring(theme.shared.fontFamily)};

    setResource(resources, L"ContentControlThemeFontFamily", font);
    setResource(resources, L"BodyFontFamily", font);
    setResource(resources, L"TextFillColorPrimaryBrush", primary);
    setResource(resources, L"TextFillColorSecondaryBrush", secondary);
    setResource(resources, L"TextFillColorTertiaryBrush", secondary);
    setResource(resources, L"DividerStrokeColorDefaultBrush", divider);
    setResource(resources, L"AccentFillColorDefaultBrush", accent);
    setResource(resources, L"AccentTextFillColorPrimaryBrush", accent);
    setResource(resources, L"ListViewItemBackgroundSelected", selection);
    setResource(resources, L"ListViewItemBackgroundSelectedPointerOver", selection);
    setResource(resources, L"ListViewItemBackgroundSelectedPressed", selection);
    setResource(resources, L"ListViewItemForegroundSelected", primary);

    root.Background(themeColorBrush(theme.shared.windowBackground));

    // A preset's own chrome takes its rounding from the window resources, so a
    // retro theme squares the Classic bars off without the frame knowing which
    // elements those bars are.
    auto const retro = theme.classic.chrome == ClassicChrome::Retro;
    setClassicChromeCornerRadius(resources, retro ? 0.0 : kClassicChromeCornerRadius);
  }

  void applySystemFrameTheme(Grid const& root)
  {
    auto const resources = root.Resources();
    clearThemeResources(resources);
    root.Background(nullptr);
    setClassicChromeCornerRadius(resources, kClassicChromeCornerRadius);
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
