// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/TextCell.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::tui::test
{
  TEST_CASE("TextCell - cellWidth measures terminal cells", "[tui][unit][text]")
  {
    CHECK(cellWidth("abc") == 3);
    CHECK(cellWidth("雨") == 2);
  }

  TEST_CASE("TextCell - panelColumnsForContent adds border and clamps to terminal", "[tui][unit][text]")
  {
    CHECK(panelColumnsForContent(10, 0) == 12);
    CHECK(panelColumnsForContent(10, 80) == 12);
    CHECK(panelColumnsForContent(10, 8) == 8);
  }

  TEST_CASE("TextCell - truncateToCellWidth respects terminal cell width", "[tui][unit][text]")
  {
    CHECK(truncateToCellWidth("abcdef", 3) == "abc");
    CHECK(truncateToCellWidth("雨abc", 1).empty());
    CHECK(truncateToCellWidth("雨abc", 2) == "雨");
    CHECK(truncateToCellWidth("雨abc", 3) == "雨a");
    CHECK(truncateToCellWidth("abc", 0).empty());
  }

  TEST_CASE("TextCell - fitCellText pads to fixed cell width", "[tui][unit][text]")
  {
    CHECK(fitCellText("a", 3) == "a  ");
    CHECK(fitCellText("a", 3, CellAlignment::Right) == "  a");
    CHECK(fitCellText("abcdef", 3) == "abc");
    CHECK(fitCellText("雨", 4) == "雨  ");
    CHECK(fitCellText("雨", 4, CellAlignment::Right) == "  雨");
  }
} // namespace ao::tui::test
