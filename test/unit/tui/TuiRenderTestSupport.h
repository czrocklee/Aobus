// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/pixel.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::tui::test
{
  struct RenderedElement final
  {
    ftxui::Screen screen;
    std::string text{};
  };

  std::string stripAnsi(std::string_view text);

  RenderedElement renderElement(ftxui::Element elementPtr, std::int32_t width, std::int32_t height);

  RenderedElement renderElementFit(ftxui::Element elementPtr, std::int32_t width = 120);

  RenderedElement renderElement(ftxui::Element elementPtr, std::int32_t width = 120);

  std::string renderText(ftxui::Element elementPtr, std::int32_t width = 120);

  std::int32_t lineIndexContaining(std::string_view text, std::string_view needle);

  // ASCII-only cell search: each byte in needle is compared to one terminal cell.
  std::optional<ftxui::Box> findTextCells(ftxui::Screen const& screen, std::string_view needle);

  void checkInteractiveSurface(ftxui::Pixel const& pixel);

  void checkDefaultSurface(ftxui::Pixel const& pixel);
} // namespace ao::tui::test
