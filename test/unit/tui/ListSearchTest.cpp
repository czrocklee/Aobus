// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/ListSearch.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include "tui/SelectionNavigation.h"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>

#include <string>
#include <vector>

namespace ao::tui::test
{
  TEST_CASE("ListSearch - the visible search hint can receive a mouse click", "[tui][regression][search][mouse]")
  {
    auto search = ListSearch{};
    auto const rendered = renderElement(search.render(ao::test::englishMessageCatalog()), 30, 1);
    auto const optHint = findTextCells(rendered.screen, "/ search");
    REQUIRE(optHint);
    REQUIRE(search.tryHandleEvent(ftxui::Event::Mouse(
      "",
      ftxui::Mouse{
        .button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = optHint->x_min, .y = optHint->y_min})));
    CHECK(search.isActive());
    REQUIRE(search.tryHandleEvent(ftxui::Event::Character("jazz")));
    CHECK(search.matches("Jazz"));
    CHECK_FALSE(search.matches("Rock"));
  }

  TEST_CASE("ListSearch - filtering preserves owner indices and rejects an empty selection", "[tui][unit][search]")
  {
    auto search = ListSearch{};
    auto const labels = std::vector<std::string>{"Rock", "Jazz", "Pop", "Jazz 2026"};
    CHECK_FALSE(search.tryHandleEvent(ftxui::Event::Character("j")));
    REQUIRE(search.tryHandleEvent(ftxui::Event::Character("/")));
    REQUIRE(search.tryHandleEvent(ftxui::Event::Character("jAzZ")));
    CHECK(search.selection(labels, 0) == 1);
    CHECK(search.selection(labels, 1, 1) == 3);
    CHECK(search.selection(labels, 3, 1) == 3);
    CHECK(search.selection(labels, 3, -1) == 1);
    REQUIRE(search.tryHandleEvent(ftxui::Event::Character("missing")));
    CHECK_FALSE(search.selection(labels, 3));
    REQUIRE(search.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(search.isActive());
    CHECK(search.selection(labels, 3, -1) == 2);
  }

  TEST_CASE("ListSearch - text navigation and list navigation have distinct owners", "[tui][unit][search]")
  {
    auto search = ListSearch{};
    REQUIRE(search.tryHandleEvent(ftxui::Event::Character("/")));
    REQUIRE(search.tryHandleEvent(ftxui::Event::Character("中文")));
    REQUIRE(search.tryHandleEvent(ftxui::Event::Home));
    REQUIRE(search.tryHandleEvent(ftxui::Event::Character("日")));
    CHECK(search.matches("日中文合集"));
    CHECK_FALSE(search.matches("中文"));
    CHECK_FALSE(search.tryHandleEvent(ftxui::Event::ArrowDown));
    CHECK_FALSE(search.tryHandleEvent(ftxui::Event::PageDown));
    CHECK_FALSE(search.tryHandleEvent(ftxui::Event::Return));
    CHECK(listNavigationDelta(ftxui::Event::PageDown, 3) == 3);
    CHECK(listNavigationDelta(ftxui::Event::PageUp, 17) == -17);
    CHECK_FALSE(listNavigationDelta(ftxui::Event::Character("j"), 3));
    CHECK(listNavigationDelta(ftxui::Event::Character("j"), 3, true) == 1);
  }
} // namespace ao::tui::test
