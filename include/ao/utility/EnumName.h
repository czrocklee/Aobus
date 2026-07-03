// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace ao::utility
{
  template<typename Enum, std::size_t N>
  using EnumNameTable = std::array<std::pair<Enum, std::string_view>, N>;

  template<typename Enum, std::size_t N>
  constexpr std::string_view enumName(EnumNameTable<Enum, N> const& names, Enum value)
  {
    for (auto const& [item, name] : names)
    {
      if (item == value)
      {
        return name;
      }
    }

    return "invalid";
  }

  template<typename Enum, std::size_t N>
  constexpr std::optional<Enum> enumFromName(EnumNameTable<Enum, N> const& names, std::string_view name)
  {
    for (auto const& [item, itemName] : names)
    {
      if (itemName == name)
      {
        return item;
      }
    }

    return std::nullopt;
  }
} // namespace ao::utility
