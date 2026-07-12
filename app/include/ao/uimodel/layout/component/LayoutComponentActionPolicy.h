// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/action/LayoutActionSlot.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  /**
   * @brief Defines which interaction slots a component supports and their default behaviors.
   */
  struct LayoutComponentActionPolicy final
  {
    LayoutActionSlotMask slotMask = 0;
    std::vector<std::pair<LayoutActionSlot, std::string>> defaultActionIds = {};

    constexpr bool isSlotAllowed(LayoutActionSlot const slot) const noexcept { return (slotMask & slotBit(slot)) != 0; }

    std::string_view defaultAction(LayoutActionSlot const slot) const
    {
      if (auto const it =
            std::ranges::find_if(defaultActionIds, [slot](auto const& entry) { return entry.first == slot; });
          it != defaultActionIds.end())
      {
        return it->second;
      }

      return {};
    }
  };

  inline LayoutComponentActionPolicy const kNoExternalActions{.slotMask = 0};

  inline LayoutComponentActionPolicy const kAllExternalActions{
    .slotMask = slotBit(LayoutActionSlot::PrimaryClick) | slotBit(LayoutActionSlot::PrimaryLongPress) |
                slotBit(LayoutActionSlot::SecondaryClick) | slotBit(LayoutActionSlot::SecondaryLongPress)};

  inline LayoutComponentActionPolicy const kExternalSecondaryActions{
    .slotMask = slotBit(LayoutActionSlot::SecondaryClick) | slotBit(LayoutActionSlot::SecondaryLongPress)};

  inline LayoutComponentActionPolicy const kExternalPrimaryActions{
    .slotMask = slotBit(LayoutActionSlot::PrimaryClick) | slotBit(LayoutActionSlot::PrimaryLongPress)};
} // namespace ao::uimodel
