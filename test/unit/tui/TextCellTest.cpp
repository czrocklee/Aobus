// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/TextCell.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace ao::tui::test
{
  namespace
  {
    /// One glyph to a reader, three to FTXUI: emoji, joiner, emoji.
    constexpr std::string_view kWomanTechnologist = "\U0001F469\u200D\U0001F4BB";
    /// Two regional indicators; half of one renders as a stray letter.
    constexpr std::string_view kJapanFlag = "\U0001F1EF\U0001F1F5";
  } // namespace

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

  TEST_CASE("TextCell - shortening handles empty and negative budgets", "[tui][unit][text]")
  {
    CHECK(truncateToCellWidth("abc", 0).empty());
    CHECK(truncateToCellWidth("abc", -1).empty());
    CHECK(ellipsizeToCellWidth("abc", 0).empty());
    CHECK(ellipsizeToCellWidth("abc", -1).empty());
    CHECK(fitCellText("abc", 0).empty());
    CHECK(fitCellText("abc", -1).empty());
  }

  TEST_CASE("TextCell - a one-cell budget cannot hold a wide glyph", "[tui][unit][text]")
  {
    CHECK(truncateToCellWidth("雨", 1).empty());
    CHECK(fitCellText("雨", 1) == " ");
    // One cell is exactly the marker, so it is all the budget can say.
    CHECK(ellipsizeToCellWidth("雨", 1) == "…");
    CHECK(cellWidth(ellipsizeToCellWidth("雨", 1)) == 1);
  }

  TEST_CASE("TextCell - ellipsizeToCellWidth reports dropped text", "[tui][unit][text]")
  {
    CHECK(ellipsizeToCellWidth("abc", 3) == "abc");
    CHECK(ellipsizeToCellWidth("abc", 8) == "abc");
    CHECK(ellipsizeToCellWidth("abcdef", 3) == "ab…");
    CHECK(ellipsizeToCellWidth("abcdef", 0).empty());
  }

  TEST_CASE("TextCell - ellipsized text never splits a glyph", "[tui][unit][text]")
  {
    CHECK(ellipsizeToCellWidth("雨雨雨", 6) == "雨雨雨");
    CHECK(ellipsizeToCellWidth("雨雨雨", 5) == "雨雨…");
    // A wide glyph cannot be halved to spend the odd cell, so the marker lands early.
    CHECK(ellipsizeToCellWidth("雨雨雨", 4) == "雨…");
    CHECK(cellWidth(ellipsizeToCellWidth("雨雨雨", 4)) <= 4);
    // The acute stays with the vowel it belongs to.
    CHECK(ellipsizeToCellWidth("e\u0301abc", 2) == "e\u0301\u2026");
  }

  TEST_CASE("TextCell - shortening keeps emoji clusters whole", "[tui][unit][text]")
  {
    // Cutting between FTXUI glyphs would emit a lone person or a dangling
    // joiner, so the cluster is kept or dropped whole.
    CHECK(truncateToCellWidth(kWomanTechnologist, 2).empty());
    CHECK(truncateToCellWidth(kWomanTechnologist, 4).empty());
    CHECK(truncateToCellWidth(kWomanTechnologist, 5) == kWomanTechnologist);
    CHECK(truncateToCellWidth(std::string{kWomanTechnologist} + "x", 6) == std::string{kWomanTechnologist} + "x");

    CHECK(truncateToCellWidth(kJapanFlag, 1).empty());
    CHECK(truncateToCellWidth(kJapanFlag, 2) == kJapanFlag);
    CHECK(truncateToCellWidth(std::string{kJapanFlag} + "x", 2) == kJapanFlag);

    // A skin tone decorates the emoji it follows and costs it no extra cells.
    CHECK(truncateToCellWidth("\U0001F44D\U0001F3FD", 1).empty());
    CHECK(truncateToCellWidth("\U0001F44D\U0001F3FD", 2) == "\U0001F44D\U0001F3FD");
  }

  TEST_CASE("TextCell - ellipsized emoji text stays well formed", "[tui][unit][text]")
  {
    auto const shortened = ellipsizeToCellWidth(std::string{kWomanTechnologist} + "abc", 4);

    // The cluster does not fit beside the marker, so only the marker survives.
    CHECK(shortened == "…");
    CHECK(ellipsizeToCellWidth(std::string{kWomanTechnologist} + "abc", 6) == std::string{kWomanTechnologist} + "…");
    CHECK_FALSE(ellipsizeToCellWidth(std::string{kWomanTechnologist} + "abc", 5).contains("\u200D"));
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
