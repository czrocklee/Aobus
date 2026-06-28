// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/track/TrackSelectionSummary.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>

namespace ao::uimodel::test
{
  TEST_CASE("trackSelectionSummaryText returns count text", "[uimodel][unit][library][track]")
  {
    CHECK(trackSelectionSummaryText(0).empty());
    CHECK(trackSelectionSummaryText(1) == "1 item selected");
    CHECK(trackSelectionSummaryText(2) == "2 items selected");
    CHECK(trackSelectionSummaryText(42) == "42 items selected");
  }

  TEST_CASE("trackSelectionSummaryText appends positive duration", "[uimodel][unit][library][track]")
  {
    // formatDuration: 200s -> "3:20", 300s -> "5:00".
    CHECK(trackSelectionSummaryText(1, std::chrono::seconds{200}) == "1 item selected (3:20)");
    CHECK(trackSelectionSummaryText(2, std::chrono::seconds{300}) == "2 items selected (5:00)");
  }

  TEST_CASE("trackSelectionSummaryText ignores non-positive duration", "[uimodel][unit][library][track]")
  {
    CHECK(trackSelectionSummaryText(2, std::chrono::milliseconds{0}) == "2 items selected");
    CHECK(trackSelectionSummaryText(2, std::nullopt) == "2 items selected");
  }

  TEST_CASE("trackSelectionSummaryText ignores duration for empty selection", "[uimodel][unit][library][track]")
  {
    CHECK(trackSelectionSummaryText(0, std::chrono::seconds{200}).empty());
  }
} // namespace ao::uimodel::test
