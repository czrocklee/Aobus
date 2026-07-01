// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::tui
{
  enum class CellAlignment : std::uint8_t
  {
    Left,
    Right,
  };

  std::string truncateToCellWidth(std::string_view value, std::int32_t width);
  std::string fitCellText(std::string_view value, std::int32_t width, CellAlignment alignment = CellAlignment::Left);
} // namespace ao::tui
