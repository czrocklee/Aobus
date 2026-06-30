// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/Render.h"

#include "tui/ShellModel.h"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::tui::test
{
  namespace
  {
    std::string renderText(ftxui::Element elementPtr)
    {
      auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(140), ftxui::Dimension::Fit(elementPtr));
      ftxui::Render(screen, elementPtr);
      return screen.ToString();
    }

    std::string renderText(ftxui::Element elementPtr, std::int32_t const width, std::int32_t const height)
    {
      auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
      ftxui::Render(screen, elementPtr);
      return screen.ToString();
    }

    std::string lineContaining(std::string_view text, std::string_view needle)
    {
      auto const position = text.find(needle);

      if (position == std::string_view::npos)
      {
        return {};
      }

      auto const lineBegin = text.rfind('\n', position);
      auto const lineEnd = text.find('\n', position);
      auto const begin = lineBegin == std::string_view::npos ? 0 : lineBegin + 1;
      auto const end = lineEnd == std::string_view::npos ? text.size() : lineEnd;
      return std::string{text.substr(begin, end - begin)};
    }
  } // namespace

  TEST_CASE("Render - help pane advertises workspace commands", "[tui][unit][render]")
  {
    auto const text = renderText(helpPane());

    CHECK(text.find("/current") != std::string::npos);
    CHECK(text.find("/view <id>") != std::string::npos);
    CHECK(text.find("/output") != std::string::npos);
  }

  TEST_CASE("Render - compact and wide status bars advertise the same shortcuts", "[tui][unit][render]")
  {
    auto shell = ShellModel{};
    auto const compact = renderText(statusBar(StatusBarViewState{.shell = &shell, .terminalColumns = 80}));
    auto const wide = renderText(statusBar(StatusBarViewState{.shell = &shell, .terminalColumns = 140}));

    CHECK(compact.find("d detail") != std::string::npos);
    CHECK(compact.find("o output") != std::string::npos);
    CHECK(compact.find("Ctrl-L current") != std::string::npos);
    CHECK(compact.find("/view id") != std::string::npos);
    CHECK(wide.find("d detail") != std::string::npos);
    CHECK(wide.find("o output") != std::string::npos);
    CHECK(wide.find("Ctrl-L current") != std::string::npos);
    CHECK(wide.find("/view id") != std::string::npos);
  }

  TEST_CASE("Render - anchored popover opens below its trigger", "[tui][unit][render]")
  {
    using namespace ftxui;

    auto const anchor = Box{.x_min = 12, .x_max = 17, .y_min = 0, .y_max = 0};
    auto const rendered = renderText(anchoredPopover(anchor, 8, 40, text("Popup") | size(WIDTH, EQUAL, 8)), 40, 4);
    auto const line = lineContaining(rendered, "Popup");

    REQUIRE_FALSE(line.empty());
    CHECK(line.find("Popup") == 12);
  }

  TEST_CASE("Render - anchored popover clamps inside the terminal width", "[tui][unit][render]")
  {
    using namespace ftxui;

    auto const anchor = Box{.x_min = 36, .x_max = 39, .y_min = 0, .y_max = 0};
    auto const rendered = renderText(anchoredPopover(anchor, 8, 40, text("Popup") | size(WIDTH, EQUAL, 8)), 40, 4);
    auto const line = lineContaining(rendered, "Popup");

    REQUIRE_FALSE(line.empty());
    CHECK(line.find("Popup") == 32);
  }
} // namespace ao::tui::test
