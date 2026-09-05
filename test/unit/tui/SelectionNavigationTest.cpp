// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/SelectionNavigation.h"

#include "test/unit/MessageCatalogTestSupport.h"

#include <catch2/catch_test_macros.hpp>

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

  TEST_CASE("SelectionNavigation - movement is bounded", "[tui][unit][selection]")
  {
    CHECK(moveSelection(0, 1, 0) == 0);
    CHECK(moveSelection(4, 1, 5) == 4);
    CHECK(moveSelection(0, -1, 5) == 0);
    CHECK(moveSelection(2, 2, 5) == 4);
    CHECK(moveSelection(2, -2, 5) == 0);
  }
} // namespace ao::tui::test
