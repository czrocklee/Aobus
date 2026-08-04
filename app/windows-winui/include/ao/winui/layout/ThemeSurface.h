// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/winui/layout/ElementKind.h>

#include <optional>
#include <string_view>

namespace ao::uimodel
{
  struct LayoutNode;
}

namespace ao::winui
{
  struct Theme;

  /// Layout field naming which themed surface a node paints itself as.
  inline constexpr std::string_view kSurfaceLayoutProp = "surface";

  /**
   * @brief Themed surface a Windows shell node paints.
   *
   * A `styleKey` carries geometry, spacing, and typography, all of which a
   * `Style` can hold and none of which changes with the theme. Colour does
   * change with the theme, and a `Style` is parsed once at window load, so the
   * preset names a slot instead and the shell resolves it against the active
   * `Theme` while the generation is being built.
   *
   * Only the slots the two built-in Windows presets paint are listed.
   */
  enum class ThemeSurface : std::uint8_t
  {
    Window,
    Surface,
    ModernNavigation,
    ModernInspector,
    ModernNowPlaying,
    ClassicToolbar,
    ClassicTree,
    ClassicStatusBar,
  };

  /// Prop spelling a preset authors for @p surface.
  std::string_view toString(ThemeSurface surface) noexcept;

  /// Surface @p name spells, or nullopt when it names none.
  std::optional<ThemeSurface> themeSurfaceFromString(std::string_view name) noexcept;

  /**
   * @brief Colour token @p theme carries for @p surface.
   *
   * Every slot resolves: the theme structure holds one token per painted
   * surface, so a slot that parsed always has a colour to hand back.
   */
  std::string_view themeSurfaceToken(Theme const& theme, ThemeSurface surface) noexcept;

  /**
   * @brief Whether @p kind owns a background a surface slot can paint.
   *
   * A `TextBlock` draws glyphs and nothing behind them, so a slot authored on
   * one would silently do nothing. Rejecting it keeps the preset honest.
   */
  bool elementKindAcceptsSurface(ElementKind kind) noexcept;

  /**
   * @brief Surface @p node authors, or nullopt when it authors none.
   *
   * A malformed or unknown slot reads as absent here; validation reports it.
   */
  std::optional<ThemeSurface> planThemeSurface(uimodel::LayoutNode const& node);
} // namespace ao::winui
