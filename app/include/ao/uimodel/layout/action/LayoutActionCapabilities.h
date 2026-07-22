// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>

namespace ao::uimodel
{
  enum class LayoutActionCapability : std::uint8_t
  {
    None = 0,
    RequiresAnchor = 1U << 0U,
    PresentsMenu = 1U << 1U
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
      auto const value = static_cast<std::uint32_t>(cap);
      return (mask & value) == value;
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
} // namespace ao::uimodel
