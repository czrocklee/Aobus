// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  enum class LayoutActionCapability : std::uint8_t
  {
    None = 0,
    RequiresAnchor = 1U << 0U,
    RequiresActiveTrack = 1U << 1U,
    RequiresFocusedView = 1U << 2U,
    PresentsMenu = 1U << 3U
  };

  struct LayoutActionCapabilities final
  {
    std::uint32_t mask = 0;

    constexpr LayoutActionCapabilities() = default;
    constexpr LayoutActionCapabilities(LayoutActionCapability cap)
      : mask{static_cast<std::uint32_t>(cap)}
    {
    }
    constexpr explicit LayoutActionCapabilities(std::uint32_t mask)
      : mask{mask}
    {
    }

    constexpr bool has(LayoutActionCapability cap) const
    {
      auto const val = static_cast<std::uint32_t>(cap);
      return (mask & val) == val;
    }

    constexpr LayoutActionCapabilities operator|(LayoutActionCapabilities other) const
    {
      return LayoutActionCapabilities{mask | other.mask};
    }

    constexpr LayoutActionCapabilities& operator|=(LayoutActionCapabilities other)
    {
      mask |= other.mask;
      return *this;
    }
  };

  constexpr LayoutActionCapabilities operator|(LayoutActionCapability lhs, LayoutActionCapability rhs)
  {
    return LayoutActionCapabilities{lhs} | LayoutActionCapabilities{rhs};
  }

  struct LayoutActionDescriptor final
  {
    std::string id;
    std::string label;
    std::string category;
    LayoutActionCapabilities capabilities = LayoutActionCapability::None;
  };

  struct LayoutActionAvailability final
  {
    bool enabled = true;
    std::string disabledReason;
  };

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

  struct LayoutActionBindingProperty final
  {
    LayoutActionSlot slot = LayoutActionSlot::PrimaryClick;
  };

  struct LayoutActionBindingContext final
  {
    LayoutActionSlot slot = LayoutActionSlot::PrimaryClick;
    bool hasAnchor = false;
    bool hasFocusedView = false;
    std::string_view componentType;
  };

  enum class LayoutActionActivationResult : std::uint8_t
  {
    Activated,
    UnknownAction,
    Disabled,
    InvalidBinding
  };

  struct LayoutActionActivationOutcome final
  {
    LayoutActionActivationResult result = LayoutActionActivationResult::UnknownAction;
    LayoutActionAvailability state = {};
  };
} // namespace ao::uimodel
