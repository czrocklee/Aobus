// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/action/LayoutActionSlot.h>

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

    std::string_view defaultAction(LayoutActionSlot slot) const;
  };

  inline LayoutComponentActionPolicy const kNoExternalActions{.slotMask = 0};

  inline LayoutComponentActionPolicy const kAllExternalActions{
    .slotMask = slotBit(LayoutActionSlot::PrimaryClick) | slotBit(LayoutActionSlot::PrimaryLongPress) |
                slotBit(LayoutActionSlot::SecondaryClick) | slotBit(LayoutActionSlot::SecondaryLongPress)};

  inline LayoutComponentActionPolicy const kExternalSecondaryActions{
    .slotMask = slotBit(LayoutActionSlot::SecondaryClick) | slotBit(LayoutActionSlot::SecondaryLongPress)};

  inline LayoutComponentActionPolicy const kExternalPrimaryActions{
    .slotMask = slotBit(LayoutActionSlot::PrimaryClick) | slotBit(LayoutActionSlot::PrimaryLongPress)};

  /**
   * @brief Every slot but the secondary long press.
   *
   * Windows raises one holding sequence per press regardless of which button
   * started it, so a secondary long press cannot be told apart from a primary
   * one there. A shared descriptor that names all four would let a document
   * validate against the catalog and then be rejected outright when the shell
   * tried to bind it.
   */
  inline LayoutComponentActionPolicy const kExternalActionsWithoutSecondaryLongPress{
    .slotMask = slotBit(LayoutActionSlot::PrimaryClick) | slotBit(LayoutActionSlot::PrimaryLongPress) |
                slotBit(LayoutActionSlot::SecondaryClick)};
} // namespace ao::uimodel
