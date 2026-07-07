// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tui::test
{
  struct RenderedElement final
  {
    ftxui::Screen screen;
    std::string text{};
  };

  inline std::string stripAnsi(std::string_view text)
  {
    auto result = std::string{};
    result.reserve(text.size());

    for (std::size_t index = 0; index < text.size(); ++index)
    {
      if (text[index] != '\x1B')
      {
        result.push_back(text[index]);
        continue;
      }

      ++index;

      while (index < text.size() && std::isalpha(static_cast<unsigned char>(text[index])) == 0 && text[index] != '\\')
      {
        ++index;
      }
    }

    return result;
  }

  inline RenderedElement renderElement(ftxui::Element elementPtr, std::int32_t const width, std::int32_t const height)
  {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
    ftxui::Render(screen, elementPtr);
    auto text = stripAnsi(screen.ToString());
    return RenderedElement{.screen = std::move(screen), .text = std::move(text)};
  }

  inline RenderedElement renderElementFit(ftxui::Element elementPtr, std::int32_t const width = 120)
  {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fit(elementPtr));
    ftxui::Render(screen, elementPtr);
    auto text = stripAnsi(screen.ToString());
    return RenderedElement{.screen = std::move(screen), .text = std::move(text)};
  }

  inline RenderedElement renderElement(ftxui::Element elementPtr, std::int32_t const width = 120)
  {
    return renderElementFit(std::move(elementPtr), width);
  }

  inline std::string renderText(ftxui::Element elementPtr, std::int32_t const width = 120)
  {
    return renderElementFit(std::move(elementPtr), width).text;
  }

  inline std::int32_t lineIndexContaining(std::string_view const text, std::string_view const needle)
  {
    auto const position = text.find(needle);

    if (position == std::string_view::npos)
    {
      return -1;
    }

    return static_cast<std::int32_t>(std::ranges::count(text.substr(0, position), '\n'));
  }

  // ASCII-only cell search: each byte in needle is compared to one terminal cell.
  inline std::optional<ftxui::Box> findTextCells(ftxui::Screen const& screen, std::string_view const needle)
  {
    if (needle.empty() || std::cmp_greater(needle.size(), screen.dimx()))
    {
      return std::nullopt;
    }

    for (std::int32_t row = 0; row < screen.dimy(); ++row)
    {
      for (std::int32_t column = 0; column <= screen.dimx() - static_cast<std::int32_t>(needle.size()); ++column)
      {
        bool matches = true;

        for (std::size_t index = 0; index < needle.size(); ++index)
        {
          if (screen.PixelAt(column + static_cast<std::int32_t>(index), row).character != std::string{needle[index]})
          {
            matches = false;
            break;
          }
        }

        if (matches)
        {
          return ftxui::Box{.x_min = column,
                            .x_max = column + static_cast<std::int32_t>(needle.size()) - 1,
                            .y_min = row,
                            .y_max = row};
        }
      }
    }

    return std::nullopt;
  }

  inline void checkInteractiveSurface(ftxui::Pixel const& pixel)
  {
    CHECK(pixel.foreground_color == ftxui::Color::Black);
    CHECK(pixel.background_color == ftxui::Color::Yellow);
    CHECK(pixel.bold);
  }

  inline void checkDefaultSurface(ftxui::Pixel const& pixel)
  {
    CHECK(pixel.foreground_color == ftxui::Color::Default);
    CHECK(pixel.background_color == ftxui::Color::Default);
  }
} // namespace ao::tui::test
