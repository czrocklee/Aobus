// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/winui/layout/ElementKind.h>

#include <cstddef>
#include <optional>
#include <string_view>

namespace ao::uimodel
{
  struct LayoutNode;
}

namespace ao::winui
{
  /**
   * @brief Layout extension this schema accepts beyond the version 1 common fields.
   *
   * Names a `Style` resource resolved against the window's `RootGrid.Resources`.
   * The style only supplies defaults: explicit document placement and
   * controller-owned semantic state win through local values.
   */
  inline constexpr std::string_view kStyleKeyLayoutProp = "styleKey";

  /**
   * @name Status reading variants
   *
   * Where a status reading sits decides whether a narrow window may drop it. A
   * status bar's own content always shows; a reading that is part of a browser
   * summary yields its space to the filter below the wide tier, which is what
   * the Windows desktop shell specification requires of the summary.
   * @{
   */
  inline constexpr std::string_view kStatusVariant = "status";
  inline constexpr std::string_view kSummaryVariant = "summary";
  /// @}

  /**
   * @brief Component types the two built-in Windows shell documents may name.
   *
   * Windows owns this schema: it is deliberately narrower than the GTK schema
   * and grows only when a Windows preset needs a type. Schema entries are already
   * expanded with the action properties their slot mask allows.
   */
  uimodel::LayoutSchema layoutSchema();

  /**
   * @brief Native element @p node constructs, or nullopt when its type is not a Windows component.
   *
   * Takes the node rather than the bare type because two Windows components pick
   * their element from an authored presentation property.
   */
  std::optional<ElementKind> componentElementKind(uimodel::LayoutNode const& node);

  /**
   * @brief Exact child count @p node's authored presentation requires, or nullopt when it fixes none.
   *
   * A presentation that swaps the constructed element also decides whether the
   * component hosts a content region, which the schema entry's child range alone
   * cannot express.
   */
  std::optional<std::size_t> presentationChildCount(uimodel::LayoutNode const& node);

  /**
   * @brief Whether a Windows component must carry a stable authored id.
   *
   * A shell switch destroys and rebuilds every element, so the components whose
   * semantic view state is reconciled into the candidate must be locatable by
   * id rather than by position in the tree.
   */
  bool componentRequiresId(std::string_view type) noexcept;
} // namespace ao::winui
