// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/winui/layout/ElementKind.h>

#include <cstdint>
#include <optional>
#include <string>

namespace ao::uimodel
{
  struct LayoutNode;
}

namespace ao::winui
{
  /// Where the resource system found a `styleKey`.
  enum class StyleScope : std::uint8_t
  {
    RootGridResources,
    ApplicationResources,
    Unresolved,
  };

  /// What the Windows shell must do with a node's authored `styleKey`.
  struct StyleLookupPlan final
  {
    std::string key;
    ElementKind elementKind = ElementKind::FrameworkElement;

    friend bool operator==(StyleLookupPlan const&, StyleLookupPlan const&) = default;
  };

  enum class StyleResolution : std::uint8_t
  {
    NoStyleAuthored,
    Applied,
    MissingKey,
    IncompatibleTarget,
  };

  /**
   * @brief Lookup @p node asks for, or nullopt when it authors no usable `styleKey`.
   *
   * Assumes the field already passed schema validation, which rejects a
   * non-string or empty key before any element is constructed.
   */
  std::optional<StyleLookupPlan> planStyleLookup(uimodel::LayoutNode const& node, ElementKind elementKind);

  /**
   * @brief Outcome of looking @p optPlan up.
   *
   * `styleKey` resolves only against the window's `RootGrid.Resources`. A key
   * that exists solely in application resources is out of scope and reported as
   * missing, so a preset cannot silently depend on framework-wide keys. A style
   * applies when its `TargetType` is the constructed element's kind or a base of
   * it, matching what WinUI itself accepts.
   */
  StyleResolution resolveStyle(std::optional<StyleLookupPlan> const& optPlan,
                               StyleScope scope,
                               std::optional<ElementKind> optStyleTarget) noexcept;
} // namespace ao::winui
