// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/action/LayoutActionSlotResolution.h>

#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  namespace
  {
    constexpr auto kAllSlots = std::array{LayoutActionSlot::PrimaryClick,
                                          LayoutActionSlot::PrimaryLongPress,
                                          LayoutActionSlot::SecondaryClick,
                                          LayoutActionSlot::SecondaryLongPress};
  } // namespace

  std::string_view actionPropForSlot(LayoutActionSlot const slot) noexcept
  {
    switch (slot)
    {
      case LayoutActionSlot::PrimaryClick: return kPrimaryActionProp;
      case LayoutActionSlot::PrimaryLongPress: return kPrimaryLongPressActionProp;
      case LayoutActionSlot::SecondaryClick: return kSecondaryActionProp;
      case LayoutActionSlot::SecondaryLongPress: return kSecondaryLongPressActionProp;
    }

    return {};
  }

  bool isActionSlotBound(LayoutComponentActionPolicy const& policy, LayoutNode const& node, LayoutActionSlot const slot)
  {
    return resolveLayoutActionId(policy, node, slot).has_value();
  }

  std::optional<std::string_view> resolveLayoutActionId(LayoutComponentActionPolicy const& policy,
                                                        LayoutNode const& node,
                                                        LayoutActionSlot const slot)
  {
    if (!policy.isSlotAllowed(slot))
    {
      return std::nullopt;
    }

    if (auto const it = node.props.find(actionPropForSlot(slot)); it != node.props.end())
    {
      auto const* const actionId = it->second.getIf<std::string>();

      if (actionId == nullptr || actionId->empty() || *actionId == "none")
      {
        return std::nullopt;
      }

      return *actionId;
    }

    auto const defaultActionId = policy.defaultAction(slot);
    return defaultActionId.empty() ? std::nullopt : std::optional{defaultActionId};
  }

  LayoutActionSlotMask boundActionSlots(LayoutComponentActionPolicy const& policy, LayoutNode const& node)
  {
    LayoutActionSlotMask mask = 0;

    for (auto const slot : kAllSlots)
    {
      if (isActionSlotBound(policy, node, slot))
      {
        mask |= slotBit(slot);
      }
    }

    return mask;
  }

  bool hasBoundActionSlot(LayoutComponentActionPolicy const& policy, LayoutNode const& node)
  {
    return boundActionSlots(policy, node) != 0;
  }
} // namespace ao::uimodel
