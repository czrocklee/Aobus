// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/NavigationPanel.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/KeymapTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include "tui/HitRegions.h"
#include "tui/ListNavigationModel.h"
#include "tui/MouseBindings.h"
#include "tui/ShellInteractionModel.h"
#include "tui/StatusBar.h"
#include <ao/rt/ListNode.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>

#include <vector>

namespace ao::tui::test
{
  TEST_CASE("NavigationPanel - docking budgets Detail and the minimum Tracks width", "[tui][unit][navigation]")
  {
    auto shell = ShellInteractionModel{};
    CHECK(shell.isNavigationEnabled());
    CHECK_FALSE(shell.isNavigationFocused());
    auto layout = navigationGeometry(100, 0, true, false, false);
    CHECK(layout.docked);
    CHECK(layout.trackColumns == 72);
    layout = navigationGeometry(80, 0, true, false, false);
    CHECK_FALSE(layout.docked);
    CHECK_FALSE(layout.drawer);
    shell.toggleNavigation(false);
    CHECK(shell.isNavigationFocused());
    CHECK(navigationGeometry(80, 0, shell.isNavigationEnabled(), shell.isNavigationFocused(), false).drawer);
    CHECK_FALSE(navigationGeometry(80, 0, true, true, true).drawer);
    layout = navigationGeometry(140, 45, true, true, false);
    CHECK(layout.drawer);
    CHECK(layout.trackColumns == 93);
    layout = navigationGeometry(160, 45, true, true, false);
    CHECK(layout.docked);
    CHECK(layout.trackColumns == 87);
    shell.switchWorkspaceFocus(false);
    CHECK_FALSE(shell.isNavigationFocused());
    CHECK(shell.isNavigationEnabled());
    shell.switchWorkspaceFocus(false);
    CHECK_FALSE(shell.isNavigationFocused());
    shell.switchWorkspaceFocus(true);
    CHECK(shell.isNavigationFocused());
    shell.switchWorkspaceFocus(true);
    CHECK_FALSE(shell.isNavigationFocused());
    CHECK(shell.isNavigationEnabled());
    shell.toggleNavigation(false);
    shell.toggleNavigation(false);
    CHECK_FALSE(shell.isNavigationEnabled());
    shell.toggleNavigation(true);
    CHECK(shell.isNavigationFocused());
    shell.toggleNavigation(true);
    CHECK_FALSE(shell.isNavigationEnabled());
    shell.switchWorkspaceFocus(true);
    CHECK_FALSE(shell.isNavigationFocused());
    CHECK_FALSE(shell.isNavigationEnabled());
  }

  TEST_CASE("NavigationPanel - translated narrow panels retain close and search targets",
            "[tui][regression][navigation]")
  {
    for (auto const* locale : {"en", "de", "zh-Hans"})
    {
      INFO(locale);
      auto const catalog = ao::test::messageCatalog(locale);
      auto model = ListNavigationModel{};
      auto const lists = std::vector<rt::ListNode>{{.id = ListId{1}, .name = "A long recognizable name"}};
      model.setTree(uimodel::buildListTreeProjection(catalog, lists), rt::kAllTracksListId);
      auto regions = NavigationHitRegions{};
      auto const rendered = renderElement(navigationPanel(catalog,
                                                          model,
                                                          rt::kAllTracksListId,
                                                          defaultKeymapPlan(),
                                                          {.columns = 20, .focused = true, .regions = &regions}),
                                          20,
                                          10);
      CHECK_FALSE(regions.panel.closeBox.IsEmpty());
      CHECK(regions.panel.closeBox.x_max < 20);
      CHECK(regions.panel.closeBox.y_max < 10);
      CHECK_FALSE(regions.panel.navigationBox.IsEmpty());
      auto const optSearch = findTextCells(rendered.screen, "/");
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
} // namespace ao::tui::test
