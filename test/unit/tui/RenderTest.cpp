// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/Render.h"

#include "tui/Model.h"
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
#include <vector>

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
    CHECK(text.find("/views") != std::string::npos);
  }

  TEST_CASE("Render - wide idle status bar keeps feedback shortcuts and selection on one row", "[tui][unit][render]")
  {
    auto shell = ShellModel{};
    auto const rendered = renderText(
      statusBar(StatusBarViewState{.statusMessage = "Ready", .trackCount = 8, .selectedTrack = 2, .shell = &shell}));

    CHECK(rendered.find("Ready") != std::string::npos);
    CHECK(rendered.find("3 / 8 tracks") != std::string::npos);
    CHECK(lineIndexContaining(rendered, "Ready") == 0);
    CHECK(lineIndexContaining(rendered, "3 / 8 tracks") == 0);
    CHECK(lineIndexContaining(rendered, "/ command") == 0);
    CHECK(rendered.find("/ command") != std::string::npos);
    CHECK(rendered.find("l lists") != std::string::npos);
    CHECK(rendered.find("v view") != std::string::npos);
    CHECK(rendered.find("d detail") != std::string::npos);
    CHECK(rendered.find("a quality") != std::string::npos);
    CHECK(rendered.find("o output") != std::string::npos);
    CHECK(rendered.find("Ctrl-L current") != std::string::npos);
    CHECK(rendered.find("q quit") != std::string::npos);
    CHECK(rendered.find("Mode:") == std::string::npos);
    CHECK(rendered.find("Filter:") == std::string::npos);
    CHECK(rendered.find("view:") == std::string::npos);
  }

  TEST_CASE("Render - narrow idle status bar wraps shortcuts below feedback", "[tui][unit][render]")
  {
    auto shell = ShellModel{};
    auto const rendered = renderText(statusBar(StatusBarViewState{
      .statusMessage = "Ready", .trackCount = 8, .selectedTrack = 2, .terminalColumns = 80, .shell = &shell}));

    CHECK(lineIndexContaining(rendered, "Ready") == 0);
    CHECK(lineIndexContaining(rendered, "3 / 8 tracks") == 0);
    CHECK(lineIndexContaining(rendered, "/ command") == 1);
    CHECK(rendered.find("q quit") != std::string::npos);
  }

  TEST_CASE("Render - status bar shows filter only when applied", "[tui][unit][render]")
  {
    auto shell = ShellModel{};
    auto const rendered = renderText(statusBar(StatusBarViewState{.statusMessage = "Quick filter matched 2 tracks",
                                                                  .trackCount = 2,
                                                                  .selectedTrack = 0,
                                                                  .filterDraft = "Aimer",
                                                                  .shell = &shell}));

    CHECK(rendered.find("Filter: Aimer") != std::string::npos);
    CHECK(rendered.find("Filter: -") == std::string::npos);
  }

  TEST_CASE("Render - status bar uses overlay-specific help for every overlay", "[tui][unit][render]")
  {
    struct Case final
    {
      Overlay overlay = Overlay::None;
      std::string_view label{};
      std::string_view hint{};
    };

    auto const cases = std::vector<Case>{
      {.overlay = Overlay::ListChooser, .label = "Lists", .hint = "l toggle  Enter open  Esc close"},
      {.overlay = Overlay::DetailPanel, .label = "Detail", .hint = "d toggle  Esc close"},
      {.overlay = Overlay::QualityPanel, .label = "Quality", .hint = "a toggle  Esc close"},
      {.overlay = Overlay::OutputDevices, .label = "Output", .hint = "o toggle  Enter select  Esc close"},
      {.overlay = Overlay::PresentationPanel, .label = "Views", .hint = "v toggle  Enter select  Esc close"},
      {.overlay = Overlay::Help, .label = "Help", .hint = "Esc close"},
    };

    for (auto const& item : cases)
    {
      auto shell = ShellModel{};
      shell.openOverlay(item.overlay);

      auto const rendered = renderText(statusBar(StatusBarViewState{.shell = &shell}));

      CHECK(rendered.find(item.label) != std::string::npos);
      CHECK(rendered.find(item.hint) != std::string::npos);
      CHECK(rendered.find("/ command") == std::string::npos);
    }
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

    auto const rendered = renderText(statusBar(StatusBarViewState{.shell = &shell}));

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

    auto const rendered = renderText(statusBar(StatusBarViewState{.shell = &shell}));

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

  TEST_CASE("Render - presentation panel renders selected and active views", "[tui][unit][render]")
  {
    auto const items = std::vector<PresentationNavItem>{
      {.id = "songs", .label = "Songs", .detail = "General-purpose song list."},
      {.id = "albums", .label = "Albums", .detail = "Grouped by album."},
    };
    auto rowBoxes = std::vector<PresentationRowBox>{};

    auto const rendered = renderElement(presentationPanel(items, "albums", 1, &rowBoxes), 48, 16);

    CHECK(rendered.text.find("Views") != std::string::npos);
    CHECK(rendered.text.find("albums") != std::string::npos);
    CHECK(rendered.text.find("* Albums") != std::string::npos);
    REQUIRE(rowBoxes.size() == 2);
    CHECK(rowBoxes[1].rowIndex == 1);
    CHECK(rendered.screen.PixelAt(rowBoxes[1].box.x_min, rowBoxes[1].box.y_min).inverted);
  }

  TEST_CASE("Render - presentation panel handles empty and out-of-range selection", "[tui][unit][render]")
  {
    auto rowBoxes = std::vector<PresentationRowBox>{};
    auto rendered = renderElement(presentationPanel({}, "", 99, &rowBoxes), 48, 16);

    CHECK(rendered.text.find("default") != std::string::npos);
    CHECK(rendered.text.find("No views available") != std::string::npos);
    CHECK(rowBoxes.empty());

    auto const items = std::vector<PresentationNavItem>{
      {.id = "songs", .label = "Songs", .detail = "General-purpose song list."},
    };
    rendered = renderElement(presentationPanel(items, "songs", 99, &rowBoxes), 48, 16);

    REQUIRE(rowBoxes.size() == 1);
    CHECK_FALSE(rendered.screen.PixelAt(rowBoxes[0].box.x_min, rowBoxes[0].box.y_min).inverted);
  }
} // namespace ao::tui::test
