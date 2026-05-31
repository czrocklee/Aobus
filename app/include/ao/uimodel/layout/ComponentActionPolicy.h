// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/ActionTypes.h>

#include <map>
#include <string>
#include <string_view>

namespace ao::uimodel::layout
{
  /**
   * @brief Defines which interaction slots a component supports and their default behaviors.
   */
  struct ComponentActionPolicy final
  {
    ActionSlotMask slotMask = 0;
    std::map<ActionSlot, std::string> defaultActionIds = {};

    constexpr bool allows(ActionSlot const slot) const { return (slotMask & slotBit(slot)) != 0; }

    std::string_view getDefault(ActionSlot const slot) const
    {
      if (auto const it = defaultActionIds.find(slot); it != defaultActionIds.end())
      {
        return it->second;
      }

      return {};
    }
  };

  inline ComponentActionPolicy const kNoExternalActions{.slotMask = 0};

  inline ComponentActionPolicy const kAllExternalActions{
    .slotMask = slotBit(ActionSlot::PrimaryClick) | slotBit(ActionSlot::PrimaryLongPress) |
                slotBit(ActionSlot::SecondaryClick) | slotBit(ActionSlot::SecondaryLongPress)};

  inline ComponentActionPolicy const kExternalSecondaryActions{.slotMask = slotBit(ActionSlot::SecondaryClick) |
                                                                           slotBit(ActionSlot::SecondaryLongPress)};

  inline ComponentActionPolicy const kExternalPrimaryActions{.slotMask = slotBit(ActionSlot::PrimaryClick) |
                                                                         slotBit(ActionSlot::PrimaryLongPress)};
} // namespace ao::uimodel::layout
