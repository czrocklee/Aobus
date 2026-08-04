// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <optional>
#include <string_view>

namespace ao::uimodel
{
  /// Authored property name that carries the action id bound to @p slot.
  std::string_view actionPropForSlot(LayoutActionSlot slot) noexcept;

  /**
   * @brief Action id resolved for @p slot, or no action when the slot is unbound.
   *
   * An authored empty string or `none` explicitly suppresses the policy
   * default. A property the policy disallows also resolves to no action.
   */
  std::optional<std::string_view> resolveLayoutActionId(LayoutComponentActionPolicy const& policy,
                                                        LayoutNode const& node,
                                                        LayoutActionSlot slot);

  /**
   * @brief Whether @p slot resolves to an action for @p node under @p policy.
   *
   * A slot resolves when @ref resolveLayoutActionId returns an action id.
   */
  bool isActionSlotBound(LayoutComponentActionPolicy const& policy, LayoutNode const& node, LayoutActionSlot slot);

  /// Mask of every slot that resolves to an action for @p node under @p policy.
  LayoutActionSlotMask boundActionSlots(LayoutComponentActionPolicy const& policy, LayoutNode const& node);

  /// Whether any slot resolves to an action, i.e. whether the node needs interaction wiring.
  bool hasBoundActionSlot(LayoutComponentActionPolicy const& policy, LayoutNode const& node);
} // namespace ao::uimodel
