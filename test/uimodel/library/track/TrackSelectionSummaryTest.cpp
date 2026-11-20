// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/track/TrackSelectionSummary.h>

#include "test/MessageCatalogTestSupport.h"
#include <ao/rt/library/LibraryChanges.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>

namespace ao::uimodel::test
{
  TEST_CASE("TrackSelectionSummary - only commits affecting stored IDs invalidate duration")
  {
    auto const selection = std::array{TrackId{7}, TrackId{2}};
    auto const reset = rt::LibraryChangeSet{.libraryReset = true};
    CHECK(shouldRefreshTrackSelectionSummary(reset, selection));
    CHECK_FALSE(shouldRefreshTrackSelectionSummary(reset, {}));
    CHECK_FALSE(shouldRefreshTrackSelectionSummary({.listsUpserted = {ListId{7}}}, selection));
    CHECK_FALSE(
      shouldRefreshTrackSelectionSummary({.tracksDeleted = {TrackId{5}}, .tracksMutated = {TrackId{1}}}, selection));
    CHECK(shouldRefreshTrackSelectionSummary({.tracksInserted = {TrackId{2}}}, selection));
    CHECK(shouldRefreshTrackSelectionSummary({.tracksDeleted = {TrackId{7}}}, selection));
    CHECK(shouldRefreshTrackSelectionSummary({.tracksMutated = {TrackId{1}, TrackId{2}}}, selection));
  }

  TEST_CASE("trackSelectionSummaryText returns count text")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    CHECK(trackSelectionSummaryText(textCatalog, 0).empty());
    CHECK(trackSelectionSummaryText(textCatalog, 1) == "1 item selected");
    CHECK(trackSelectionSummaryText(textCatalog, 2) == "2 items selected");
    CHECK(trackSelectionSummaryText(textCatalog, 42) == "42 items selected");
  }

  TEST_CASE("trackSelectionSummaryText appends positive duration")
  {
    // formatDuration: 200s -> "3:20", 300s -> "5:00".
    auto const& textCatalog = ao::test::englishMessageCatalog();
    CHECK(trackSelectionSummaryText(textCatalog, 1, std::chrono::seconds{200}) == "1 item selected (3:20)");
    CHECK(trackSelectionSummaryText(textCatalog, 2, std::chrono::seconds{300}) == "2 items selected (5:00)");
  }

  TEST_CASE("trackSelectionSummaryText ignores non-positive duration")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    CHECK(trackSelectionSummaryText(textCatalog, 2, std::chrono::milliseconds{0}) == "2 items selected");
    CHECK(trackSelectionSummaryText(textCatalog, 2, std::nullopt) == "2 items selected");
  }

  TEST_CASE("trackSelectionSummaryText ignores duration for empty selection")
  {
    CHECK(trackSelectionSummaryText(ao::test::englishMessageCatalog(), 0, std::chrono::seconds{200}).empty());
  }
} // namespace ao::uimodel::test
