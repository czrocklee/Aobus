// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TextCell.h"

#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ao::tui
{
  std::string truncateToCellWidth(std::string_view const value, std::int32_t const width)
  {
    if (width <= 0)
    {
      return {};
    }

    auto result = std::string{};
    std::int32_t used = 0;

    for (auto const& glyph : ftxui::Utf8ToGlyphs(std::string{value}))
    {
      auto const glyphWidth = static_cast<std::int32_t>(ftxui::string_width(glyph));

      if (glyphWidth == 0)
      {
        result += glyph;
        continue;
      }

      if (used + glyphWidth > width)
      {
        break;
      }

      result += glyph;
      used += glyphWidth;
    }

    return result;
  }

  std::string fitCellText(std::string_view const value, std::int32_t const width, CellAlignment const alignment)
  {
    auto result = truncateToCellWidth(value, width);
    auto const padding = std::max(0, width - static_cast<std::int32_t>(ftxui::string_width(result)));

    if (padding <= 0)
    {
      return result;
    }

    if (alignment == CellAlignment::Right)
    {
      result.insert(0, static_cast<std::size_t>(padding), ' ');
      return result;
    }

    result.append(static_cast<std::size_t>(padding), ' ');
    return result;
  }
} // namespace ao::tui
