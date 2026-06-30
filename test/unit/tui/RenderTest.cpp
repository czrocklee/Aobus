// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/Render.h"

#include "tui/ShellModel.h"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include <string>

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
  } // namespace

  TEST_CASE("Render - help pane advertises workspace commands", "[tui][unit][render]")
  {
    auto const text = renderText(helpPane());

    CHECK(text.find("/current") != std::string::npos);
    CHECK(text.find("/view <id>") != std::string::npos);
  }

  TEST_CASE("Render - compact and wide status bars advertise the same shortcuts", "[tui][unit][render]")
  {
    auto shell = ShellModel{};
    auto const compact = renderText(statusBar(StatusBarViewState{.shell = &shell, .terminalColumns = 80}));
    auto const wide = renderText(statusBar(StatusBarViewState{.shell = &shell, .terminalColumns = 140}));

    CHECK(compact.find("d detail") != std::string::npos);
    CHECK(compact.find("Ctrl-L current") != std::string::npos);
    CHECK(compact.find("/view id") != std::string::npos);
    CHECK(wide.find("d detail") != std::string::npos);
    CHECK(wide.find("Ctrl-L current") != std::string::npos);
    CHECK(wide.find("/view id") != std::string::npos);
  }
} // namespace ao::tui::test
