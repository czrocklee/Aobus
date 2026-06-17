// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/track/SelectionSummary.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>

namespace ao::uimodel::track::test
{
  TEST_CASE("selectionSummaryText - count text", "[uimodel][track][selection]")
  {
    CHECK(selectionSummaryText(0).empty());
    CHECK(selectionSummaryText(1) == "1 item selected");
    CHECK(selectionSummaryText(2) == "2 items selected");
    CHECK(selectionSummaryText(42) == "42 items selected");
  }

  TEST_CASE("selectionSummaryText - appends positive duration", "[uimodel][track][selection]")
  {
    // formatDuration: 200s -> "3:20", 300s -> "5:00".
    CHECK(selectionSummaryText(1, std::chrono::seconds{200}) == "1 item selected (3:20)");
    CHECK(selectionSummaryText(2, std::chrono::seconds{300}) == "2 items selected (5:00)");
  }

  TEST_CASE("selectionSummaryText - non-positive duration adds nothing", "[uimodel][track][selection]")
  {
    CHECK(selectionSummaryText(2, std::chrono::milliseconds{0}) == "2 items selected");
    CHECK(selectionSummaryText(2, std::nullopt) == "2 items selected");
  }

  TEST_CASE("selectionSummaryText - empty selection ignores duration", "[uimodel][track][selection]")
  {
    CHECK(selectionSummaryText(0, std::chrono::seconds{200}).empty());
  }
} // namespace ao::uimodel::track::test
