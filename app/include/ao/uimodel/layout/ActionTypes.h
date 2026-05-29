// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::uimodel::layout
{
  enum class ActionCapability : std::uint8_t
  {
    None = 0,
    RequiresAnchor = 1U << 0U,
    RequiresActiveTrack = 1U << 1U,
    RequiresFocusedView = 1U << 2U,
    PresentsMenu = 1U << 3U
  };

  struct ActionCapabilities final
  {
    std::uint32_t mask = 0;

    constexpr ActionCapabilities() = default;
    constexpr ActionCapabilities(ActionCapability cap)
      : mask{static_cast<std::uint32_t>(cap)}
    {
    }
    constexpr explicit ActionCapabilities(std::uint32_t mask)
      : mask{mask}
    {
    }

    constexpr bool has(ActionCapability cap) const
    {
      auto const val = static_cast<std::uint32_t>(cap);
      return (mask & val) == val;
    }

    constexpr ActionCapabilities operator|(ActionCapabilities other) const
    {
      return ActionCapabilities{mask | other.mask};
    }

    constexpr ActionCapabilities& operator|=(ActionCapabilities other)
    {
      mask |= other.mask;
      return *this;
    }
  };

  constexpr ActionCapabilities operator|(ActionCapability lhs, ActionCapability rhs)
  {
    return ActionCapabilities{lhs} | ActionCapabilities{rhs};
  }

  struct ActionDescriptor final
  {
    std::string id;
    std::string label;
    std::string category;
    ActionCapabilities capabilities = ActionCapability::None;
  };

  struct ActionState final
  {
    bool enabled = true;
    std::string disabledReason;
  };

  enum class ActionSlot : std::uint8_t
  {
    PrimaryClick,
    PrimaryLongPress,
    SecondaryClick,
    SecondaryLongPress,
    MenuItem,
    Shortcut
  };

  struct ActionBindingProperty final
  {
    ActionSlot slot = ActionSlot::PrimaryClick;
  };

  struct ActionBindingContext final
  {
    ActionSlot slot = ActionSlot::PrimaryClick;
    bool hasAnchor = false;
    bool hasFocusedView = false;
    std::string_view componentType;
  };

  enum class ActionActivationResult : std::uint8_t
  {
    Activated,
    UnknownAction,
    Disabled,
    InvalidBinding
  };

  struct ActionActivationOutcome final
  {
    ActionActivationResult result = ActionActivationResult::UnknownAction;
    ActionState state = {};
  };
}
