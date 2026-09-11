// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/AnchoredOverlay.h"

#include "tui/MouseBindings.h"
#include "tui/Style.h"

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
    template<typename Anchor>
    concept FollowingAnchor = requires(Anchor&& box) {
      followingAnchoredOverlay(ftxui::Element{},
                               std::forward<Anchor>(box),
                               AnchoredOverlayPlacement::Below,
                               AnchoredOverlaySize{},
                               AnchoredOverlayTerminal{});
    };

    static_assert(FollowingAnchor<ftxui::Box&>);
    static_assert(FollowingAnchor<ftxui::Box const&>);
    static_assert(!FollowingAnchor<ftxui::Box>);
    static_assert(!FollowingAnchor<ftxui::Box const>);

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
    CHECK(lineIndexContaining(rendered, "Popup") == 3);
  }

  TEST_CASE("AnchoredOverlay - clears one-cell gutter for wide glyphs next to the edge", "[tui][unit][overlay]")
  {
    using namespace ftxui;

    auto backgroundPtr = vbox({
      filler() | size(HEIGHT, EQUAL, 3),
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
    CHECK(screen.PixelAt(11, 3).character == " ");
  }

  TEST_CASE("AnchoredOverlay - halo clears all sides without swallowing the trigger or mouse targets",
            "[tui][regression][overlay]")
  {
    using namespace ftxui;

    for (auto const placement : {AnchoredOverlayPlacement::Above, AnchoredOverlayPlacement::Below})
    {
      for (std::int32_t const triggerColumn : {0, 12, 39})
      {
        auto backgroundRows = Elements{};

        for (std::int32_t row = 0; row < 20; ++row)
        {
          backgroundRows.push_back(text(std::string(40, '#')));
        }

        auto regions = PanelMouseRegions{};
        auto overlayPtr = anchoredOverlay(mousePanel(text("Body") | border | size(WIDTH, EQUAL, 12), regions, 0),
                                          anchor(triggerColumn, triggerColumn, 10),
                                          placement,
                                          {.columns = 12, .rows = 3},
                                          {.columns = 40, .rows = 20});
        auto const screen = renderScreen(dbox({vbox(std::move(backgroundRows)), std::move(overlayPtr)}), 40, 20);
        auto const box = regions.box;
        REQUIRE(box.x_min >= 1);
        REQUIRE(box.x_max <= 38);
        REQUIRE(box.y_min >= 1);
        REQUIRE(box.y_max <= 18);
        CHECK(screen.PixelAt(box.x_min, box.y_min).character == "╭");
        CHECK(screen.PixelAt(box.x_max, box.y_min).character == "╮");
        CHECK(screen.PixelAt(box.x_min, box.y_max).character == "╰");
        CHECK(screen.PixelAt(box.x_max, box.y_max).character == "╯");
        CHECK(screen.PixelAt(triggerColumn, 10).character == "#");

        for (auto column = box.x_min - 1; column <= box.x_max + 1; ++column)
        {
          CHECK(screen.PixelAt(column, box.y_min - 1).character == " ");
          CHECK(screen.PixelAt(column, box.y_max + 1).character == " ");
        }

        for (auto row = box.y_min; row <= box.y_max; ++row)
        {
          CHECK(screen.PixelAt(box.x_min - 1, row).character == " ");
          CHECK(screen.PixelAt(box.x_max + 1, row).character == " ");
        }
      }
    }
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
    CHECK(line.find("Popup") == 31);
  }

  TEST_CASE("AnchoredOverlay - constrained width preserves the halo and border", "[tui][regression][overlay]")
  {
    using namespace ftxui;
    auto regions = PanelMouseRegions{};
    auto const screen =
      renderScreen(anchoredOverlay(mousePanel(text("Body") | border | size(WIDTH, EQUAL, 20), regions),
                                   anchor(0, 0, 0),
                                   AnchoredOverlayPlacement::Below,
                                   {.columns = 20, .rows = 3},
                                   {.columns = 20, .rows = 10}),
                   20,
                   10);
    REQUIRE(regions.box.x_min == 1);
    REQUIRE(regions.box.x_max == 18);
    CHECK(screen.PixelAt(19, regions.box.y_min).character == " ");
    CHECK(screen.PixelAt(18, regions.box.y_min).character == "╮");
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
    CHECK(lineIndexContaining(rendered, "Popup") == 2);
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

    CHECK(lineIndexContaining(rendered, "Popup") == 4);
  }

  TEST_CASE("AnchoredOverlay - follows a frame button placed later in the same render after resizing",
            "[tui][regression][overlay]")
  {
    using namespace ftxui;
    auto button = kEmptyMouseBox;
    auto popupBox = kEmptyMouseBox;

    for (std::int32_t const columns : {80, 140, 48})
    {
      button = kEmptyMouseBox;
      auto backgroundPtr =
        hbox({filler() | size(WIDTH, EQUAL, columns / 10),
              style::titledPanel("", filler(), {.leftFooter = {.label = "List", .value = "Current", .box = &button}}) |
                flex});
      auto popupPtr =
        followingAnchoredOverlay(text("Chooser") | size(WIDTH, EQUAL, 18) | size(HEIGHT, EQUAL, 4) | reflect(popupBox),
                                 button,
                                 AnchoredOverlayPlacement::Above,
                                 {.columns = 18, .rows = 4},
                                 {.columns = columns, .rows = 24},
                                 {.fallbackToBottom = true});
      auto const screen = renderScreen(dbox({std::move(backgroundPtr), std::move(popupPtr)}), columns, 24);
      CHECK(screen.ToString().contains("Chooser"));
      REQUIRE_FALSE(button.IsEmpty());
      CHECK(popupBox.x_min == button.x_min);
      CHECK(popupBox.y_max == button.y_min - 2);
    }
  }

  TEST_CASE("AnchoredOverlay - playback trigger updates before a lower overlay layer is placed",
            "[tui][regression][overlay]")
  {
    using namespace ftxui;
    auto button = kEmptyMouseBox;
    auto popupBox = kEmptyMouseBox;

    for (std::int32_t const columns : {80, 140, 48})
    {
      // Retain the previous frame's box to exercise a paused window resize.
      auto playbackPtr = hbox({filler() | size(WIDTH, EQUAL, columns / 3), text("Output") | reflect(button), filler()});
      auto popupPtr =
        followingAnchoredOverlay(text("Devices") | size(WIDTH, EQUAL, 12) | size(HEIGHT, EQUAL, 4) | reflect(popupBox),
                                 button,
                                 AnchoredOverlayPlacement::Below,
                                 {.columns = 12, .rows = 4},
                                 {.columns = columns, .rows = 24},
                                 {.overlayLayerTopRows = 1});
      auto const screen =
        renderScreen(vbox({std::move(playbackPtr), dbox({filler(), std::move(popupPtr)}) | flex}), columns, 24);
      CHECK(screen.ToString().contains("Devices"));
      CHECK(popupBox.x_min == button.x_min);
      CHECK(popupBox.y_min == button.y_max + 2);
    }
  }

  TEST_CASE("AnchoredOverlay - notification trigger follows the status row on the first resized frame",
            "[tui][regression][overlay]")
  {
    using namespace ftxui;
    auto button = kEmptyMouseBox;
    auto popupBox = kEmptyMouseBox;

    for (std::int32_t const rows : {24, 40, 18})
    {
      auto backgroundPtr = vbox({filler(), hbox({text("Activity") | reflect(button), filler()})});
      auto popupPtr = followingAnchoredOverlay(
        text("Notifications") | size(WIDTH, EQUAL, 18) | size(HEIGHT, EQUAL, 4) | reflect(popupBox),
        button,
        AnchoredOverlayPlacement::Above,
        {.columns = 18, .rows = 4},
        {.columns = 80, .rows = rows},
        {.fallbackToBottom = true});
      auto const screen = renderScreen(dbox({std::move(backgroundPtr), std::move(popupPtr)}), 80, rows);
      CHECK(screen.ToString().contains("Notifications"));
      CHECK(popupBox.y_max == button.y_min - 2);
    }
  }
} // namespace ao::tui::test
