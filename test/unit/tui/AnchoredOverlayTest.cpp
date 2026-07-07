// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/AnchoredOverlay.h"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace ao::tui::test
{
  namespace
  {
    ftxui::Screen renderScreen(ftxui::Element elementPtr, std::int32_t const width, std::int32_t const height)
    {
      auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
      ftxui::Render(screen, elementPtr);
      return screen;
    }

    std::string renderText(ftxui::Element elementPtr, std::int32_t const width, std::int32_t const height)
    {
      auto screen = renderScreen(std::move(elementPtr), width, height);
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

    std::int32_t lineIndexContaining(std::string_view const text, std::string_view const needle)
    {
      auto const position = text.find(needle);

      if (position == std::string_view::npos)
      {
        return -1;
      }

      std::int32_t result = 0;

      for (auto const ch : text.substr(0, position))
      {
        if (ch == '\n')
        {
          ++result;
        }
      }

      return result;
    }

    ftxui::Element popup()
    {
      using namespace ftxui;
      return text("Popup") | size(WIDTH, EQUAL, 8);
    }

    ftxui::Box anchor(std::int32_t const xMin, std::int32_t const xMax, std::int32_t const y)
    {
      return ftxui::Box{.x_min = xMin, .x_max = xMax, .y_min = y, .y_max = y};
    }
  } // namespace

  TEST_CASE("AnchoredOverlay - below placement converts root anchor into render layer coordinates",
            "[tui][unit][overlay]")
  {
    auto const rendered = renderText(anchoredOverlay(popup(),
                                                     anchor(12, 17, 2),
                                                     AnchoredOverlayPlacement::Below,
                                                     AnchoredOverlaySize{.columns = 8, .rows = 1},
                                                     AnchoredOverlayTerminal{.columns = 40, .rows = 6},
                                                     AnchoredOverlayOptions{.overlayLayerTopRows = 1}),
                                     40,
                                     6);

    auto const line = lineContaining(rendered, "Popup");

    REQUIRE_FALSE(line.empty());
    CHECK(line.find("Popup") == 12);
    CHECK(lineIndexContaining(rendered, "Popup") == 2);
  }

  TEST_CASE("AnchoredOverlay - clears one-cell gutter for wide glyphs next to the edge", "[tui][unit][overlay]")
  {
    using namespace ftxui;

    auto backgroundPtr = vbox({
      filler() | size(HEIGHT, EQUAL, 2),
      hbox({
        filler() | size(WIDTH, EQUAL, 11),
        text("界"),
        filler(),
      }),
      filler(),
    });
    auto overlayPtr = anchoredOverlay(popup(),
                                      anchor(12, 17, 1),
                                      AnchoredOverlayPlacement::Below,
                                      AnchoredOverlaySize{.columns = 8, .rows = 1},
                                      AnchoredOverlayTerminal{.columns = 40, .rows = 5});

    auto const screen = renderScreen(dbox({std::move(backgroundPtr), std::move(overlayPtr)}), 40, 5);
    auto const text = screen.ToString();
    auto const line = lineContaining(text, "Popup");

    REQUIRE_FALSE(line.empty());
    CHECK(line.find("Popup") == 12);
    CHECK(screen.PixelAt(11, 2).character == " ");
  }

  TEST_CASE("AnchoredOverlay - placement clamps inside terminal width", "[tui][unit][overlay]")
  {
    auto const rendered = renderText(anchoredOverlay(popup(),
                                                     anchor(36, 39, 0),
                                                     AnchoredOverlayPlacement::Below,
                                                     AnchoredOverlaySize{.columns = 8, .rows = 1},
                                                     AnchoredOverlayTerminal{.columns = 40, .rows = 4}),
                                     40,
                                     4);
    auto const line = lineContaining(rendered, "Popup");

    REQUIRE_FALSE(line.empty());
    CHECK(line.find("Popup") == 32);
  }

  TEST_CASE("AnchoredOverlay - above placement opens over its trigger", "[tui][unit][overlay]")
  {
    auto const rendered = renderText(anchoredOverlay(popup(),
                                                     anchor(12, 17, 6),
                                                     AnchoredOverlayPlacement::Above,
                                                     AnchoredOverlaySize{.columns = 8, .rows = 2},
                                                     AnchoredOverlayTerminal{.columns = 40, .rows = 8},
                                                     AnchoredOverlayOptions{.overlayLayerTopRows = 1}),
                                     40,
                                     8);
    auto const line = lineContaining(rendered, "Popup");

    REQUIRE_FALSE(line.empty());
    CHECK(line.find("Popup") == 12);
    CHECK(lineIndexContaining(rendered, "Popup") == 3);
  }

  TEST_CASE("AnchoredOverlay - empty anchor can fall back to terminal bottom", "[tui][unit][overlay]")
  {
    auto const rendered = renderText(anchoredOverlay(popup(),
                                                     ftxui::Box{},
                                                     AnchoredOverlayPlacement::Above,
                                                     AnchoredOverlaySize{.columns = 8, .rows = 2},
                                                     AnchoredOverlayTerminal{.columns = 40, .rows = 8},
                                                     AnchoredOverlayOptions{.fallbackToBottom = true}),
                                     40,
                                     8);

    CHECK(lineIndexContaining(rendered, "Popup") == 5);
  }
} // namespace ao::tui::test
