// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/Render.h"

#include "tui/ShellModel.h"
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tui::test
{
  namespace
  {
    struct RenderedElement final
    {
      ftxui::Screen screen;
      std::string text{};
    };

    RenderedElement renderElement(ftxui::Element elementPtr, std::int32_t const width, std::int32_t const height)
    {
      auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
      ftxui::Render(screen, elementPtr);
      auto text = screen.ToString();
      return RenderedElement{.screen = std::move(screen), .text = std::move(text)};
    }

    std::string renderText(ftxui::Element elementPtr)
    {
      auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(140), ftxui::Dimension::Fit(elementPtr));
      ftxui::Render(screen, elementPtr);
      return screen.ToString();
    }

    std::string renderText(ftxui::Element elementPtr, std::int32_t const width, std::int32_t const height)
    {
      return renderElement(std::move(elementPtr), width, height).text;
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

    std::int32_t lineIndexContaining(std::string_view const text, std::string_view const needle)
    {
      auto const position = text.find(needle);

      if (position == std::string_view::npos)
      {
        return -1;
      }

      return static_cast<std::int32_t>(std::ranges::count(text.substr(0, position), '\n'));
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

  TEST_CASE("Render - command status shows inline completion suffix", "[tui][unit][render]")
  {
    auto shell = ShellModel{};
    shell.beginCommand("A");
    shell.setCommandCompletion(rt::CompletionResult{
      .replaceBegin = 0,
      .replaceEnd = 1,
      .items = {rt::CompletionItem{.displayText = "Aimer", .insertText = "Aimer", .detail = "artist"}},
    });

    auto const rendered = renderText(statusBar(StatusBarViewState{.shell = &shell, .terminalColumns = 140}));

    CHECK(rendered.find("/A") != std::string::npos);
    CHECK(rendered.find("imer") != std::string::npos);
    CHECK(rendered.find("Tab complete") != std::string::npos);
  }

  TEST_CASE("Render - command status suppresses full-command ghost for empty drafts", "[tui][unit][render]")
  {
    auto shell = ShellModel{};
    shell.beginCommand();
    shell.setCommandCompletion(rt::CompletionResult{
      .replaceBegin = 0,
      .replaceEnd = 0,
      .items = {rt::CompletionItem{.displayText = "/output", .insertText = "output", .detail = "output device"}},
    });

    auto const rendered = renderText(statusBar(StatusBarViewState{.shell = &shell, .terminalColumns = 140}));

    CHECK(rendered.find("/output") == std::string::npos);
    CHECK(rendered.find("Tab complete") != std::string::npos);
  }

  TEST_CASE("Render - command completion panel renders item details", "[tui][unit][render]")
  {
    auto const completion = rt::CompletionResult{
      .items =
        {
          rt::CompletionItem{.displayText = "/view", .insertText = "view ", .detail = "track view"},
          rt::CompletionItem{.displayText = "Aimer", .insertText = "Aimer", .detail = "artist"},
        },
    };

    auto const rendered = renderElement(commandCompletionPanel(completion, 1), 48, 5);

    CHECK(rendered.text.find("/view") != std::string::npos);
    CHECK(rendered.text.find("track view") != std::string::npos);
    CHECK(rendered.text.find("Aimer") != std::string::npos);
    CHECK(rendered.text.find("artist") != std::string::npos);
    CHECK_FALSE(rendered.screen.PixelAt(1, 1).inverted);
    CHECK(rendered.screen.PixelAt(1, 2).inverted);
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

  TEST_CASE("Render - anchored popover above opens over its trigger", "[tui][unit][render]")
  {
    using namespace ftxui;

    auto const anchor = Box{.x_min = 12, .x_max = 17, .y_min = 5, .y_max = 5};
    auto const rendered =
      renderText(anchoredPopoverAbove(anchor, 8, 40, 8, 2, text("Popup") | size(WIDTH, EQUAL, 8)), 40, 8);
    auto const line = lineContaining(rendered, "Popup");

    REQUIRE_FALSE(line.empty());
    CHECK(line.find("Popup") == 12);
    CHECK(lineIndexContaining(rendered, "Popup") == 3);
  }
} // namespace ao::tui::test
