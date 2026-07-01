// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/TextCell.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::tui::test
{
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
