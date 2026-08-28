// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace ao::uimodel
{
  struct LayoutNode;

  /**
   * @brief How a node asks to be aligned within the space its parent gives it.
   *
   * The authored vocabulary is the document's, not a toolkit's: every frontend
   * reads the same four words and maps them onto whatever its own toolkit
   * spells them as. That mapping is the only part that differs, which is why it
   * stays in the frontend and this does not.
   */
  enum class LayoutAlignment : std::uint8_t
  {
    Fill,
    Start,
    End,
    Center,
  };

  /**
   * @brief One node's version 1 common layout fields, parsed.
   *
   * Every frontend interprets the same seven fields, so they are read once here
   * rather than re-parsed per toolkit. What a frontend does with the result is
   * its own: expansion is a child property in GTK and a row or column
   * definition in WinUI, and only the frontend knows which.
   *
   * An unset optional means the document authored nothing, so whatever default
   * the frontend's own styling supplies stays in effect. Expansion is optional
   * for the same reason it is not merely false: GTK derives a widget's
   * expansion from its children until something states it, so an authored
   * `hexpand: false` and an absent one are different instructions.
   *
   * Visibility is optional for a sharper reason. Components hide themselves for
   * their own semantic reasons - a volume control with no volume to show, an
   * undo bar with nothing to undo - and a document that said nothing about
   * visibility must not overrule them.
   *
   * A size request additionally carries an authored flag because a negative
   * value explicitly clears a native minimum while an absent field preserves
   * the frontend default; both cases have no positive minimum value.
   */
  struct LayoutPlacement final
  {
    std::optional<bool> optHorizontalExpand{};
    std::optional<bool> optVerticalExpand{};
    std::optional<LayoutAlignment> optHorizontalAlignment{};
    std::optional<LayoutAlignment> optVerticalAlignment{};
    std::optional<double> optMinWidth{};
    std::optional<double> optMinHeight{};
    bool widthRequestAuthored = false;
    bool heightRequestAuthored = false;
    std::optional<bool> optAuthoredVisible{};

    friend bool operator==(LayoutPlacement const&, LayoutPlacement const&) = default;
  };

  /// Whether @p name is one of the version 1 common layout fields every frontend interprets.
  bool isCommonLayoutProp(std::string_view name) noexcept;

  /// Alignment @p name spells, or nullopt when it spells none.
  std::optional<LayoutAlignment> layoutAlignmentFromString(std::string_view name) noexcept;

  /**
   * @brief Placement for @p node, assuming its layout fields already passed schema validation.
   *
   * Malformed values cannot reach a constructed element: validation rejects the
   * whole candidate first, so this mapping ignores anything it cannot interpret.
   */
  LayoutPlacement planLayoutPlacement(LayoutNode const& node);

  /**
   * @brief Whether an element is shown, given its placement and the owning controller's semantic state.
   *
   * An authored `visible: false` always collapses. Runtime state may additionally
   * hide an authored-visible element, but never reveals one the document hid.
   */
  constexpr bool isPlacedElementVisible(LayoutPlacement const& placement, bool const runtimeVisible) noexcept
  {
    return placement.optAuthoredVisible.value_or(true) && runtimeVisible;
  }
} // namespace ao::uimodel
