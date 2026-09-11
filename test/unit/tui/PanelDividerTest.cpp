// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/tui/EventControllerTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include "tui/HitRegions.h"
#include "tui/NavigationPanel.h"
#include "tui/Render.h"
#include "tui/Style.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>

#include <cstdint>
#include <utility>

namespace ao::tui::test
{
  TEST_CASE("PanelDivider - separate borders preserve padding and one-cell toggle targets", "[tui][regression][render]")
  {
    using namespace ftxui;

    for (bool const separate : {false, true})
    {
      for (bool const hovered : {false, true})
      {
        for (std::int32_t const columns : {100, 160})
        {
          CAPTURE(separate, hovered, columns);
          auto hits = HitRegions{};
          auto trackFrame = Box{};
          auto detailFrame = Box{};
          auto workspacePtr = style::popupPanel("", text("Tracks") | flex) | reflect(trackFrame);
          auto detailPtr = style::popupPanel("", text("Detail") | flex) | size(WIDTH, EQUAL, 40) | reflect(detailFrame);
          auto rootPtr = dockDetailPane(std::move(workspacePtr),
                                        std::move(detailPtr),
                                        40,
                                        &hits.detailToggleBox,
                                        hovered,
                                        {.separateBorders = separate, .hoverBox = &hits.detailDividerBox});
          rootPtr = dockNavigationPanel(style::popupPanel("", text("Lists") | flex) | size(WIDTH, EQUAL, 26),
                                        std::move(rootPtr),
                                        26,
                                        &hits.navigationPinBox,
                                        hovered,
                                        {.separateBorders = separate, .hoverBox = &hits.navigationDividerBox});
          auto const rendered = renderElement(std::move(rootPtr), columns, 16);
          INFO(rendered.text);
          CHECK(trackFrame.x_min == (separate ? 26 : 25));
          CHECK(trackFrame.x_max == columns - (separate ? 41 : 40));
          CHECK(detailFrame.x_min == columns - 40);
          CHECK(detailFrame.x_max == columns - 1);
          auto const optLists = findTextCells(rendered.screen, "Lists");
          auto const optTracks = findTextCells(rendered.screen, "Tracks");
          auto const optDetail = findTextCells(rendered.screen, "Detail");
          REQUIRE(optLists);
          REQUIRE(optTracks);
          REQUIRE(optDetail);
          CHECK(optLists->x_min == 2);
          CHECK(optTracks->x_min == trackFrame.x_min + 2);
          CHECK(optDetail->x_min == detailFrame.x_min + 2);
          CHECK(hits.navigationPinBox.x_min == 25);
          CHECK(hits.detailToggleBox.x_min == columns - 40);

          for (auto const& box : {hits.navigationPinBox, hits.detailToggleBox})
          {
            CHECK(box.x_min == box.x_max);
            CHECK(box.y_min == box.y_max);
            CHECK(rendered.screen.PixelAt(box.x_min, box.y_min).background_color ==
                  (hovered ? Color{Color::Yellow} : Color{Color::Default}));
          }

          for (auto const& box : {hits.navigationDividerBox, hits.detailDividerBox})
          {
            CHECK(box.x_max - box.x_min == (separate ? 1 : 0));
            CHECK(box.y_min == 0);
            CHECK(box.y_max == 15);
            CHECK(rendered.screen.PixelAt(box.x_min, 0).character == (separate ? "╮" : "┬"));
            CHECK(rendered.screen.PixelAt(box.x_max, 15).character == (separate ? "╰" : "┴"));
            CHECK(rendered.screen.PixelAt(box.x_min, 2).character == "│");
            CHECK(rendered.screen.PixelAt(box.x_max, 2).character == "│");
            checkDefaultSurface(rendered.screen.PixelAt(box.x_min - 1, 2));
            checkDefaultSurface(rendered.screen.PixelAt(box.x_max + 1, 2));
            CHECK((rendered.screen.PixelAt(box.x_min, 2).foreground_color != Color{Color::Default}) == hovered);
          }

          CHECK(hits.hitTestButton(hits.navigationDividerBox.x_max, 2).hoveredButton ==
                HoveredButton::NavigationToggle);
          CHECK(hits.hitTestButton(hits.detailDividerBox.x_min, 2).hoveredButton == HoveredButton::DetailToggle);
          CHECK(hits.hitTestButton(hits.navigationDividerBox.x_max, 2, {.isOverlayActive = true}).hoveredButton ==
                HoveredButton::None);
          CHECK(hits.hitTestButton(hits.detailDividerBox.x_min, 2, {.isOverlayActive = true}).hoveredButton ==
                HoveredButton::None);
          hits.clearFrameLocalRows();
          CHECK(hits.navigationDividerBox.IsEmpty());
          CHECK(hits.detailDividerBox.IsEmpty());
        }
      }
    }
  }

  TEST_CASE("PanelDivider - docking budgets include each separate border", "[tui][unit][navigation]")
  {
    CHECK(navigationGeometry(100, 0, true).docked);
    CHECK_FALSE(navigationGeometry(100, 0, true, true).docked);
    auto const lists = navigationGeometry(101, 0, true, true);
    CHECK(lists.docked);
    CHECK(lists.trackColumns == 72);
    CHECK(navigationGeometry(144, 45, true).docked);
    CHECK_FALSE(navigationGeometry(145, 45, true, true).docked);
    auto const both = navigationGeometry(146, 45, true, true);
    CHECK(both.docked);
    CHECK(both.trackColumns == 72);
    CHECK(navigationGeometry(146, 45, false, true).trackColumns == 98);
  }

  TEST_CASE("PanelDivider - hover-only arrows restore border strokes without changing targets",
            "[tui][regression][render]")
  {
    using namespace ftxui;

    for (bool const separate : {false, true})
    {
      auto hits = HitRegions{};
      auto render = [&](bool const hovered)
      {
        auto rootPtr =
          dockDetailPane(style::popupPanel("", text("Tracks") | flex),
                         style::popupPanel("", text("Detail") | flex) | size(WIDTH, EQUAL, 40),
                         40,
                         &hits.detailToggleBox,
                         hovered,
                         {.separateBorders = separate, .hoverBox = &hits.detailDividerBox, .revealOnHover = true});
        return renderElement(
          dockNavigationPanel(
            style::popupPanel("", text("Lists") | flex) | size(WIDTH, EQUAL, 26),
            std::move(rootPtr),
            26,
            &hits.navigationPinBox,
            hovered,
            {.separateBorders = separate, .hoverBox = &hits.navigationDividerBox, .revealOnHover = true}),
          140,
          16);
      };
      auto const idle = render(false);
      auto const left = hits.navigationPinBox;
      auto const right = hits.detailToggleBox;
      REQUIRE_FALSE(left.IsEmpty());
      REQUIRE_FALSE(right.IsEmpty());
      CHECK(idle.screen.PixelAt(left.x_min, left.y_min).character == "│");
      CHECK(idle.screen.PixelAt(right.x_min, right.y_min).character == "│");
      auto const hovered = render(true);
      CHECK(hits.navigationPinBox == left);
      CHECK(hits.detailToggleBox == right);
      CHECK(hovered.screen.PixelAt(left.x_min, left.y_min).character == "‹");
      CHECK(hovered.screen.PixelAt(right.x_min, right.y_min).character == "›");
      checkInteractiveSurface(hovered.screen.PixelAt(left.x_min, left.y_min));
      checkInteractiveSurface(hovered.screen.PixelAt(right.x_min, right.y_min));
      auto const cleared = render(false);
      CHECK(cleared.text == idle.text);
    }
  }

  TEST_CASE("PanelDivider - concealed collapsed arrows remain discoverable from their side borders",
            "[tui][regression][mouse]")
  {
    using namespace ftxui;
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.shell.setNavigationPinned(false);
    fixture.hitRegions.navigationLayout = navigationGeometry(140, 0, false);
    auto render = [&]
    {
      auto rootPtr = collapsedDetailPane(style::popupPanel("", text("Tracks") | flex),
                                         fixture.hitRegions.detailToggleBox,
                                         events.hoveredButton() == HoveredButton::DetailToggle,
                                         true,
                                         &fixture.hitRegions.detailDividerBox);
      return renderElement(collapsedNavigationPanel(std::move(rootPtr),
                                                    fixture.hitRegions.navigationPinBox,
                                                    events.hoveredButton() == HoveredButton::NavigationToggle,
                                                    true,
                                                    &fixture.hitRegions.navigationDividerBox),
                           140,
                           16);
    };
    auto mouse = [&](std::int32_t const x, std::int32_t const y, bool const pressed)
    {
      return events.tryHandleEvent(Event::Mouse("",
                                                {.button = pressed ? Mouse::Left : Mouse::None,
                                                 .motion = pressed ? Mouse::Pressed : Mouse::Moved,
                                                 .x = x,
                                                 .y = y}));
    };

    for (bool const detail : {false, true})
    {
      auto const idle = render();
      auto const box = detail ? fixture.hitRegions.detailToggleBox : fixture.hitRegions.navigationPinBox;
      CHECK(idle.screen.PixelAt(box.x_min, box.y_min).character == "│");
      REQUIRE(mouse(box.x_min, 3, false));
      CHECK(events.hoveredButton() == (detail ? HoveredButton::DetailToggle : HoveredButton::NavigationToggle));
      auto const hovered = render();
      CHECK(hovered.screen.PixelAt(box.x_min, box.y_min).character == (detail ? "‹" : "›"));
      checkInteractiveSurface(hovered.screen.PixelAt(box.x_min, box.y_min));
      mouse(box.x_min, 3, true);
      CHECK_FALSE(fixture.shell.isDetailVisible());
      CHECK_FALSE(fixture.shell.isNavigationPinned());
      REQUIRE(mouse(70, 3, false));
      auto const cleared = render();
      CHECK(cleared.screen.PixelAt(box.x_min, box.y_min).character == "│");
      REQUIRE(mouse(box.x_min, box.y_min, true));
      CHECK(fixture.shell.isDetailVisible() == detail);
      CHECK(fixture.shell.isNavigationPinned() == !detail);
      fixture.shell.setNavigationPinned(false);
    }
  }

  TEST_CASE("PanelDivider - hovering a divider does not make its whole line a collapse target",
            "[tui][regression][mouse]")
  {
    using namespace ftxui;
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.shell.setNavigationPinned(true);
    fixture.shell.toggleDetail();
    fixture.hitRegions.navigationLayout = navigationGeometry(160, 40, true, true);
    fixture.hitRegions.navigationPinBox = {.x_min = 25, .x_max = 25, .y_min = 8, .y_max = 8};
    fixture.hitRegions.navigationDividerBox = {.x_min = 25, .x_max = 26, .y_min = 0, .y_max = 15};
    fixture.hitRegions.detailToggleBox = {.x_min = 120, .x_max = 120, .y_min = 8, .y_max = 8};
    fixture.hitRegions.detailDividerBox = {.x_min = 119, .x_max = 120, .y_min = 0, .y_max = 15};
    fixture.hitRegions.navigation.panel.box = {.x_min = 0, .x_max = 25, .y_min = 0, .y_max = 15};
    fixture.hitRegions.detailPanel.box = {.x_min = 120, .x_max = 159, .y_min = 0, .y_max = 15};
    auto mouse = [&](std::int32_t x, std::int32_t y, bool pressed)
    {
      return events.tryHandleEvent(Event::Mouse("",
                                                {.button = pressed ? Mouse::Left : Mouse::None,
                                                 .motion = pressed ? Mouse::Pressed : Mouse::Moved,
                                                 .x = x,
                                                 .y = y}));
    };
    REQUIRE(mouse(25, 2, false));
    CHECK(events.hoveredButton() == HoveredButton::NavigationToggle);
    mouse(26, 2, false);
    CHECK(events.hoveredButton() == HoveredButton::NavigationToggle);
    mouse(26, 2, true);
    events.tryHandleEvent(Event::Mouse("", {.button = Mouse::Left, .motion = Mouse::Released, .x = 26, .y = 2}));
    CHECK(fixture.shell.isNavigationPinned());
    REQUIRE(mouse(120, 2, false));
    CHECK(events.hoveredButton() == HoveredButton::DetailToggle);
    mouse(119, 2, false);
    CHECK(events.hoveredButton() == HoveredButton::DetailToggle);
    mouse(119, 2, true);
    events.tryHandleEvent(Event::Mouse("", {.button = Mouse::Left, .motion = Mouse::Released, .x = 119, .y = 2}));
    CHECK(fixture.shell.isDetailVisible());
    REQUIRE(mouse(120, 8, true));
    CHECK_FALSE(fixture.shell.isDetailVisible());
    CHECK(fixture.hitRegions.detailDividerBox.IsEmpty());
    REQUIRE(mouse(25, 8, true));
    CHECK_FALSE(fixture.shell.isNavigationPinned());
  }
} // namespace ao::tui::test
