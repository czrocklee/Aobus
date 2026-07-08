// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::uimodel
{
  enum class LayoutActionSlot : std::uint8_t
  {
    PrimaryClick,
    PrimaryLongPress,
    SecondaryClick,
    SecondaryLongPress,
    MenuItem,
    Shortcut
  };

  using LayoutActionSlotMask = std::uint32_t;

  constexpr LayoutActionSlotMask slotBit(LayoutActionSlot const slot)
  {
    return 1U << static_cast<std::uint8_t>(slot);
  }
} // namespace ao::uimodel
