// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/SelectionNavigation.h"

#include "test/unit/MessageCatalogTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/box.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace ao::tui::test
{
  TEST_CASE("SelectionNavigation - clamps to available items", "[tui][unit][selection]")
  {
    CHECK(clampSelection(0, 0) == 0);
    CHECK(clampSelection(4, 3) == 2);
    CHECK(clampSelection(1, 3) == 1);
  }

  TEST_CASE("SelectionNavigation - summary is one-based and bounded", "[tui][unit][selection]")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    CHECK(selectionSummary(textCatalog, 0, 0) == "0 tracks");
    CHECK(selectionSummary(textCatalog, 1, 0) == "1 / 1 track");
    CHECK(selectionSummary(textCatalog, 12, 0) == "1 / 12 tracks");
    CHECK(selectionSummary(textCatalog, 12, 99) == "12 / 12 tracks");
    CHECK(selectionSummary(textCatalog, 12, -4) == "1 / 12 tracks");
    CHECK(selectionSummary(textCatalog, 12, 0, 3) == "3 marked · 1 / 12 tracks");
    CHECK(selectionSummary(textCatalog, 12, 0, 3, true) == "VISUAL · 3 marked · 1 / 12 tracks");
    CHECK(selectionSummary(textCatalog, 12, 0, 0, true) == "VISUAL · 1 / 12 tracks");
  }

  TEST_CASE("SelectionNavigation - page steps and optional Vim keys", "[tui][unit][selection]")
  {
    CHECK(listNavigationDelta(ftxui::Event::PageDown, 3) == 3);
    CHECK(listNavigationDelta(ftxui::Event::PageUp, 17) == -17);
    CHECK_FALSE(listNavigationDelta(ftxui::Event::Character("j"), 3));
    CHECK(listNavigationDelta(ftxui::Event::Character("j"), 3, true) == 1);
    CHECK(listNavigationDelta(ftxui::Event::ArrowUp, 3) == -1);
    CHECK(listNavigationDelta(ftxui::Event::ArrowDown, 3) == 1);
    CHECK_FALSE(listNavigationDelta(ftxui::Event::Character("k"), 3));
    CHECK(listNavigationDelta(ftxui::Event::Character("k"), 3, true) == -1);
    CHECK(listNavigationDelta(ftxui::Event::Home, 3) == -std::numeric_limits<std::int32_t>::max());
    CHECK(listNavigationDelta(ftxui::Event::End, 3) == std::numeric_limits<std::int32_t>::max());
    CHECK_FALSE(listNavigationDelta(ftxui::Event::Character("x"), 3, true));
    CHECK(listNavigationDelta(ftxui::Event::PageUp, 0) == -1);
    CHECK(listNavigationDelta(ftxui::Event::PageDown, -4) == 1);
  }

  TEST_CASE("SelectionNavigation - movement is bounded", "[tui][unit][selection]")
  {
    CHECK(moveSelection(0, 1, 0) == 0);
    CHECK(moveSelection(4, 1, 5) == 4);
    CHECK(moveSelection(0, -1, 5) == 0);
    CHECK(moveSelection(2, 2, 5) == 4);
    CHECK(moveSelection(2, -2, 5) == 0);
  }

  TEST_CASE("SelectionNavigation - measured viewport determines page size with empty fallback",
            "[tui][unit][selection]")
  {
    auto const emptyViewport = ftxui::Box{.x_max = -1, .y_max = -1};
    REQUIRE(emptyViewport.IsEmpty());
    CHECK(navigationPageRows(emptyViewport) == 10);
    CHECK(navigationPageRows(ftxui::Box{}) == 1);
    CHECK(navigationPageRows(ftxui::Box{.x_min = 0, .x_max = 20, .y_min = 2, .y_max = 6}) == 5);
    CHECK(navigationPageRows(ftxui::Box{.x_min = 0, .x_max = 20, .y_min = 6, .y_max = 6}) == 1);
  }

  TEST_CASE("SelectionNavigation - wide signed movement saturates at both endpoints", "[tui][unit][selection]")
  {
    constexpr auto kMaxIndex = std::numeric_limits<std::int32_t>::max();
    constexpr auto kMinIndex = std::numeric_limits<std::int32_t>::min();
    CHECK(moveSelection(kMaxIndex - 2, kMaxIndex, static_cast<std::size_t>(kMaxIndex) + 500) == kMaxIndex);
    CHECK(moveSelection(3, kMinIndex, 10) == 0);
    CHECK(moveSelection(kMinIndex, kMaxIndex, 10) == 0);
  }
} // namespace ao::tui::test
