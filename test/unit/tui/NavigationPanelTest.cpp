// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/NavigationPanel.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/KeymapTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include "tui/HitRegions.h"
#include "tui/Keymap.h"
#include "tui/ListNavigationModel.h"
#include "tui/MouseBindings.h"
#include "tui/ShellInteractionModel.h"
#include "tui/StatusBar.h"
#include "tui/Style.h"
#include <ao/rt/ListNode.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/pixel.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  TEST_CASE("NavigationPanel - docking budgets Detail and the minimum Tracks width", "[tui][unit][navigation]")
  {
    auto shell = ShellInteractionModel{};
    CHECK(shell.isNavigationPinned());
    CHECK_FALSE(shell.isNavigationFocused());
    auto layout = navigationGeometry(102, 0, true);
    CHECK(layout.docked);
    CHECK(layout.trackColumns == 74);
    CHECK(navigationGeometry(100, 0, true).trackColumns == 72);
    CHECK(navigationGeometry(100, 0, true).docked);
    CHECK_FALSE(navigationGeometry(99, 0, true).docked);
    layout = navigationGeometry(80, 0, true);
    CHECK_FALSE(layout.docked);
    shell.toggleNavigation(false);
    CHECK_FALSE(shell.isNavigationFocused());
    CHECK(shell.overlay() == Overlay::ListChooser);
    shell.toggleNavigation(false);
    CHECK(shell.overlay() == Overlay::None);
    layout = navigationGeometry(140, 45, true);
    CHECK_FALSE(layout.docked);
    CHECK(layout.trackColumns == 93);
    layout = navigationGeometry(160, 45, true);
    CHECK(layout.docked);
    CHECK(layout.trackColumns == 88);
    shell.switchWorkspaceFocus(false);
    CHECK_FALSE(shell.isNavigationFocused());
    CHECK(shell.isNavigationPinned());
    shell.switchWorkspaceFocus(false);
    CHECK_FALSE(shell.isNavigationFocused());
    shell.switchWorkspaceFocus(true);
    CHECK(shell.isNavigationFocused());
    shell.switchWorkspaceFocus(true);
    CHECK_FALSE(shell.isNavigationFocused());
    CHECK(shell.isNavigationPinned());
    shell.toggleNavigation(false);
    shell.toggleNavigation(false);
    CHECK(shell.isNavigationPinned());
    CHECK_FALSE(shell.isNavigationFocused());
    shell.toggleNavigation(true);
    shell.toggleNavigation(true);
    CHECK(shell.isNavigationFocused());
    CHECK(shell.isNavigationPinned());
    shell.toggleNavigationPin();
    CHECK_FALSE(shell.isNavigationFocused());
    CHECK_FALSE(shell.isNavigationPinned());
    shell.switchWorkspaceFocus(true);
    CHECK_FALSE(shell.isNavigationFocused());
    shell.toggleNavigation(true);
    CHECK(shell.overlay() == Overlay::ListChooser);
    CHECK_FALSE(navigationGeometry(140, 0, false).docked);
    shell.toggleNavigationPin();
    CHECK(shell.isNavigationPinned());
  }

  TEST_CASE("NavigationPanel - docked panes share one border without shifting content hit targets",
            "[tui][regression][navigation]")
  {
    using namespace ftxui;
    auto const& catalog = ao::test::englishMessageCatalog();
    auto model = ListNavigationModel{};
    model.setTree(uimodel::buildListTreeProjection(catalog, {}), rt::kAllTracksListId);

    for (std::int32_t const columns : {101, 140})
    {
      auto lists = std::vector<rt::ListNode>{};

      if (columns == 101)
      {
        for (std::uint32_t index = 1; index <= 12; ++index)
        {
          lists.push_back({.id = ListId{index}, .name = "Another list"});
        }
      }

      model.setTree(uimodel::buildListTreeProjection(catalog, lists), rt::kAllTracksListId);
      auto regions = NavigationHitRegions{};
      auto trackBox = Box{};
      auto workspaceBox = Box{};
      auto navigationPtr = navigationPanel(
        catalog, model, rt::kAllTracksListId, {.columns = kNavigationColumns, .focused = true, .regions = &regions});
      auto workspacePtr = style::popupPanel("", text("Track cell") | flex | reflect(trackBox)) | reflect(workspaceBox);
      auto const rendered = renderElement(
        dockNavigationPanel(std::move(navigationPtr), std::move(workspacePtr), kNavigationColumns), columns, 10);
      INFO(rendered.text);
      CHECK(regions.panel.box.x_max == 25);
      CHECK(workspaceBox.x_min == 25);
      CHECK(trackBox.x_min == 27);
      CHECK(trackBox.x_max == columns - 3);
      CHECK(rendered.screen.PixelAt(25, 0).character == "┬");
      CHECK(rendered.screen.PixelAt(25, 9).character == "┴");

      for (std::int32_t row = 1; row < 9; ++row)
      {
        CHECK(rendered.screen.PixelAt(25, row).character == "│");
      }

      REQUIRE_FALSE(regions.rows.empty());
      auto const selectedRow = regions.rows.front().row;
      CHECK(selectedRow.x_min == 2);
      CHECK(selectedRow.x_max == 23);
      CHECK(rendered.screen.PixelAt(1, selectedRow.y_min).background_color == Color::Default);
      CHECK(rendered.screen.PixelAt(2, selectedRow.y_min).background_color == Color::Yellow);
      CHECK(rendered.screen.PixelAt(23, selectedRow.y_min).background_color == Color::Yellow);
      CHECK(rendered.screen.PixelAt(24, selectedRow.y_min).background_color == Color::Default);
      auto const& scrollbar = rendered.screen.PixelAt(24, selectedRow.y_min).character;
      CHECK((!scrollbar.empty() && scrollbar != " ") == (columns == 101));
      auto const optMarker = findTextCells(rendered.screen, "* ");
      REQUIRE(optMarker);
      CHECK(optMarker->x_min == 2);
    }
  }

  TEST_CASE("NavigationPanel - translated active search retains input targets", "[tui][regression][navigation]")
  {
    for (auto const* locale : {"en", "de", "zh-Hans"})
    {
      INFO(locale);
      auto const catalog = ao::test::messageCatalog(locale);
      auto model = ListNavigationModel{};
      auto const lists = std::vector<rt::ListNode>{{.id = ListId{1}, .name = "A long recognizable name"}};
      model.setTree(uimodel::buildListTreeProjection(catalog, lists), rt::kAllTracksListId);
      REQUIRE(model.trySearchEvent(ftxui::Event::Character("/")));
      REQUIRE(model.trySearchEvent(ftxui::Event::Character("long")));
      auto regions = NavigationHitRegions{};
      auto const rendered = renderElement(
        navigationPanel(catalog, model, rt::kAllTracksListId, {.columns = 20, .focused = true, .regions = &regions}),
        20,
        10);
      CHECK_FALSE(regions.panel.navigationBox.IsEmpty());
      auto const optSearch = findTextCells(rendered.screen, "long");
      REQUIRE(optSearch);
      REQUIRE(model.trySearchEvent(ftxui::Event::Mouse("",
                                                       {.button = ftxui::Mouse::Left,
                                                        .motion = ftxui::Mouse::Pressed,
                                                        .x = optSearch->x_min,
                                                        .y = optSearch->y_min})));
      CHECK(model.search().isActive());
      model.clearSearch();
      model.search().invalidateMouseRegions();
      CHECK_FALSE(model.trySearchEvent(ftxui::Event::Mouse("",
                                                           {.button = ftxui::Mouse::Left,
                                                            .motion = ftxui::Mouse::Pressed,
                                                            .x = optSearch->x_min,
                                                            .y = optSearch->y_min})));
    }
  }

  TEST_CASE("NavigationPanel - cover overlap includes Kitty clearing halo and excludes hidden targets",
            "[tui][regression][navigation][cover-art]")
  {
    auto const cover = ftxui::Box{.x_min = 60, .x_max = 79, .y_min = 3, .y_max = 15};
    CHECK_FALSE(hasCoverIntersection(cover, kEmptyMouseBox));
    CHECK_FALSE(hasCoverIntersection(cover, {.x_min = 0, .x_max = 25, .y_min = 2, .y_max = 20}));
    CHECK(hasCoverIntersection(cover, {.x_min = 59, .x_max = 59, .y_min = 4, .y_max = 5}));
    CHECK(hasCoverIntersection(cover, {.x_min = 20, .x_max = 65, .y_min = 10, .y_max = 20}));
    CHECK_FALSE(hasCoverIntersection(cover, {.x_min = 20, .x_max = 65, .y_min = 17, .y_max = 20}));
  }

  TEST_CASE("NavigationPanel - search status describes local ownership rather than tree keys",
            "[tui][regression][navigation]")
  {
    auto shell = ShellInteractionModel{};
    shell.focusNavigation();
    auto const rendered = renderText(statusBar(ao::test::englishMessageCatalog(),
                                               {.terminalColumns = 120, .navigationSearching = true, .shell = &shell},
                                               defaultKeymapPlan()),
                                     120);
    CHECK(rendered.contains("Esc clear"));
    CHECK_FALSE(rendered.contains("Esc tracks"));
    CHECK_FALSE(rendered.contains("tree"));
  }

  TEST_CASE("NavigationPanel - search only takes a row while active and Escape restores the full tree",
            "[tui][regression][navigation][search]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();
    auto model = ListNavigationModel{};
    model.setTree(uimodel::buildListTreeProjection(catalog, {}), rt::kAllTracksListId);
    auto regions = NavigationHitRegions{};
    auto render = [&]
    {
      return renderElement(
        navigationPanel(catalog, model, rt::kAllTracksListId, {.focused = true, .regions = &regions}), 26, 12);
    };
    auto const initial = render();
    CHECK_FALSE(initial.text.contains("/ search"));
    REQUIRE_FALSE(regions.rows.empty());
    auto const initialRow = regions.rows.front().row.y_min;
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("/")));
    render();
    CHECK(regions.rows.front().row.y_min == initialRow + 1);
    REQUIRE(model.trySearchEvent(ftxui::Event::Character("missing")));
    render();
    CHECK(regions.rows.empty());
    REQUIRE(model.trySearchEvent(ftxui::Event::Escape));
    auto const restored = render();
    CHECK_FALSE(model.search().isActive());
    CHECK_FALSE(restored.text.contains("/ search"));
    REQUIRE_FALSE(regions.rows.empty());
    CHECK(regions.rows.front().row.y_min == initialRow);
  }

  TEST_CASE("NavigationPanel - docked expression follows selection without footer chrome",
            "[tui][regression][navigation]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();
    auto model = ListNavigationModel{};
    auto const lists = std::vector<rt::ListNode>{
      {.id = ListId{1}, .name = "Favorite", .expression = "#fav"}, {.id = ListId{2}, .name = "Empty"}};
    model.setTree(uimodel::buildListTreeProjection(catalog, lists), ListId{1});
    REQUIRE(model.trySelect(ListId{1}));
    auto regions = NavigationHitRegions{};
    auto render = [&]
    {
      return renderElement(
        navigationPanel(catalog, model, ListId{1}, {.columns = 26, .focused = true, .regions = &regions}), 26, 12);
    };
    auto const caption = render();
    auto const optExpression = findTextCells(caption.screen, "#fav");
    REQUIRE(optExpression);
    CHECK(optExpression->x_min == regions.rows.front().row.x_min);
    CHECK(optExpression->y_min == 10);
    CHECK(caption.screen.PixelAt(optExpression->x_min, optExpression->y_min).dim);
    CHECK_FALSE(caption.text.contains("Tab"));
    CHECK_FALSE(caption.text.contains("tracks"));

    for (std::int32_t row = 1; row < 11; ++row)
    {
      CHECK(caption.screen.PixelAt(2, row).character != "─");
    }

    auto const viewportBottom = regions.panel.navigationBox.y_max;
    REQUIRE(model.trySelect(ListId{2}));
    auto const empty = render();
    CHECK_FALSE(empty.text.contains("#fav"));
    CHECK(regions.panel.navigationBox.y_max == viewportBottom + 1);
    CHECK(regions.rows.front().row.y_min == 1);
  }

  TEST_CASE("NavigationPanel - long expressions wrap into two dim rows with explicit truncation",
            "[tui][regression][navigation]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();
    auto model = ListNavigationModel{};
    auto const lists =
      std::vector<rt::ListNode>{{.id = ListId{1}, .name = "Music", .expression = "#收藏 #鋼琴 #古典 #現場 #其他"}};
    model.setTree(uimodel::buildListTreeProjection(catalog, lists), ListId{1});
    REQUIRE(model.trySelect(ListId{1}));
    auto const rendered =
      renderElement(navigationPanel(catalog, model, ListId{1}, {.columns = 18, .focused = true}), 18, 12);
    INFO(rendered.text);
    auto const optFirst = findTextCells(rendered.screen, "#收藏 #鋼琴");
    auto const optSecond = findTextCells(rendered.screen, "#古典");
    auto const optEllipsis = findTextCells(rendered.screen, "…");
    REQUIRE(optFirst);
    REQUIRE(optSecond);
    REQUIRE(optEllipsis);
    CHECK(optFirst->y_min == 9);
    CHECK(optSecond->y_min == 10);
    CHECK(optFirst->x_min == optSecond->x_min);
    CHECK(optEllipsis->y_min == optSecond->y_min);
    CHECK_FALSE(rendered.text.contains("#其他"));
    CHECK(rendered.screen.PixelAt(optFirst->x_min, optFirst->y_min).dim);
    CHECK(rendered.screen.PixelAt(optSecond->x_min, optSecond->y_min).dim);
  }

  TEST_CASE("NavigationPanel - expression controls flatten without corrupting Unicode", "[tui][regression][navigation]")
  {
    using namespace std::string_view_literals;
    // NOLINTNEXTLINE(misc-include-cleaner) -- MSVC include-cleaner cannot map sv to the included <string_view>.
    constexpr auto kWomanTechnologist = "\U0001F469\u200D\U0001F4BB"sv;
    auto const expression = std::string{"\u0301#a\t中\r\n\u0085#b\x7F"
                                        "文 "} +
                            std::string{kWomanTechnologist} + " #尾 #更多";
    auto const& catalog = ao::test::englishMessageCatalog();
    auto model = ListNavigationModel{};
    auto const lists = std::vector<rt::ListNode>{{.id = ListId{1}, .name = "Music", .expression = expression}};
    model.setTree(uimodel::buildListTreeProjection(catalog, lists), ListId{1});
    REQUIRE(model.trySelect(ListId{1}));
    auto const rendered =
      renderElement(navigationPanel(catalog, model, ListId{1}, {.columns = 18, .focused = true}), 18, 12);
    INFO(rendered.text);
    auto const optFirst = findTextCells(rendered.screen, "#a 中 #b文");
    auto const optEmoji = findTextCells(rendered.screen, kWomanTechnologist);
    auto const optTail = findTextCells(rendered.screen, "#尾");
    auto const optEllipsis = findTextCells(rendered.screen, "…");
    REQUIRE(optFirst);
    REQUIRE(optEmoji);
    REQUIRE(optTail);
    REQUIRE(optEllipsis);
    CHECK(optFirst->y_min + 1 == optEmoji->y_min);
    CHECK(optEmoji->y_min == optTail->y_min);
    CHECK(optTail->y_min == optEllipsis->y_min);
    CHECK_FALSE(rendered.text.contains("#更多"));
    REQUIRE(model.rows().size() == 2);
    REQUIRE(model.rows().back().id == ListId{1});
    CHECK(model.rows().back().detail == expression);
  }

  TEST_CASE("NavigationPanel - Tracks advertises focus only for a docked list using the current binding",
            "[tui][regression][navigation]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();
    auto shell = ShellInteractionModel{};
    auto state = StatusBarViewState{.terminalColumns = 80, .navigationDocked = true, .shell = &shell};
    CHECK(renderText(statusBar(catalog, state, defaultKeymapPlan()), 80).contains("Tab lists"));
    auto keymap = uimodel::KeymapModel{defaultKeymap()};
    REQUIRE(keymap.tryUnbind("tui.workspace.switchFocus", *uimodel::KeyChord::parse("Tab")));
    REQUIRE(keymap.tryUnbind("tui.workspace.switchFocus", *uimodel::KeyChord::parse("Shift+Tab")));
    REQUIRE(keymap.tryBind("tui.workspace.switchFocus", *uimodel::KeyChord::parse("F4")));
    auto const plan = KeymapPlan{keymap};
    CHECK(renderText(statusBar(catalog, state, plan), 80).contains("F4 lists"));
    state.filterDraft = "a long filter expression that consumes the remaining status width";
    CHECK(renderText(statusBar(catalog, state, plan), 80).contains("F4 lists"));
    state.navigationDocked = false;
    CHECK_FALSE(renderText(statusBar(catalog, state, plan), 80).contains("F4 lists"));
  }

  TEST_CASE("NavigationPanel - global return hint follows executable focus bindings", "[tui][regression][navigation]")
  {
    auto shell = ShellInteractionModel{};
    shell.focusNavigation();
    auto keymap = uimodel::KeymapModel{defaultKeymap()};
    REQUIRE(keymap.tryUnbind("tui.workspace.switchFocus", *uimodel::KeyChord::parse("Tab")));
    REQUIRE(keymap.tryUnbind("tui.workspace.switchFocus", *uimodel::KeyChord::parse("Shift+Tab")));
    REQUIRE(keymap.tryBind("tui.workspace.switchFocus", *uimodel::KeyChord::parse("F4")));
    auto const plan = KeymapPlan{keymap};
    auto const normal =
      renderText(statusBar(ao::test::englishMessageCatalog(), {.terminalColumns = 80, .shell = &shell}, plan), 80);
    CHECK(normal.contains("F4 tracks"));
    auto const search = renderText(
      statusBar(
        ao::test::englishMessageCatalog(), {.terminalColumns = 80, .navigationSearching = true, .shell = &shell}, plan),
      80);
    CHECK(search.contains("Tab tracks"));
    CHECK(search.contains("Esc clear"));
    CHECK_FALSE(search.contains("F4 tracks"));
  }
} // namespace ao::tui::test
