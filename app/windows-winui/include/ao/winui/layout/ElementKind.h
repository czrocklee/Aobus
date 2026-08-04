// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace ao::winui
{
  /**
   * @brief Native element a Windows shell component constructs.
   *
   * The WinUI frontend builds one of these XAML types per registered component
   * type. The enumeration exists so style-target compatibility and placement
   * rules can be decided and tested without a XAML host: it names the same
   * inheritance edges WinUI uses when it accepts a `Style` on an element.
   *
   * Only the kinds the two built-in Windows presets construct are listed.
   */
  enum class ElementKind : std::uint8_t
  {
    FrameworkElement,
    Panel,
    Grid,
    Border,
    TextBlock,
    Control,
    ContentControl,
    ButtonBase,
    Button,
    ItemsControl,
    ListView,
    ScrollViewer,
    Slider,
    AutoSuggestBox,
    NavigationView,
    TreeView,
    MenuBar,
  };

  /// Immediate base kind, or nullopt for the root of the lattice.
  std::optional<ElementKind> elementBase(ElementKind kind) noexcept;

  /// Whether @p kind is @p base or derives from it, i.e. whether a `Style` targeting @p base applies.
  bool isElementKindDerivedFrom(ElementKind kind, ElementKind base) noexcept;

  /// XAML type name, matching the `TargetType` spelling a style declares.
  std::string_view toString(ElementKind kind) noexcept;

  /// Kind for a `TargetType` name reported by the resource system, or nullopt when it is outside this vocabulary.
  std::optional<ElementKind> elementKindFromString(std::string_view name) noexcept;
} // namespace ao::winui
