// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::tui
{
  inline constexpr std::int32_t kPanelBorderColumns = 2;
  /// The marker an ellipsized string ends with, counted in the width budget.
  inline constexpr std::string_view kCellEllipsis = "\u2026";

  enum class CellAlignment : std::uint8_t
  {
    Left,
    Right,
  };

  std::int32_t cellWidth(std::string_view value);
  std::int32_t panelColumnsForContent(std::int32_t contentColumns, std::int32_t terminalColumns);
  std::string truncateToCellWidth(std::string_view value, std::int32_t width);
  /**
   * @brief @p value shortened to @p width cells, ending in @ref kCellEllipsis
   *        when anything was dropped.
   *
   * Terminal clipping cuts a value without saying so and can split a wide
   * glyph in half; this states the loss instead. Text that already fits is
   * returned unchanged, and a budget too narrow for the marker falls back to
   * plain truncation rather than overflowing the column.
   */
  std::string ellipsizeToCellWidth(std::string_view value, std::int32_t width);
  std::string fitCellText(std::string_view value, std::int32_t width, CellAlignment alignment = CellAlignment::Left);
} // namespace ao::tui
