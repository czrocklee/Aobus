// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/TerminalTrackColumnLayout.h"

#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

namespace ao::tui::test
{
  TEST_CASE("TerminalTrackColumnLayout - applies persisted order visibility and cell widths",
            "[tui][unit][track-columns]")
  {
    auto const presentation = rt::TrackPresentationSpec{
      .id = "terminal",
      .visibleFields =
        {
          rt::TrackField::Title,
          rt::TrackField::Artist,
          rt::TrackField::Album,
          rt::TrackField::Duration,
        },
    };
    auto const stored = std::vector{
      uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 13},
      uimodel::TrackColumnState{.field = rt::TrackField::Album, .visible = false},
      uimodel::TrackColumnState{.field = rt::TrackField::Artist, .weight = 1.0},
    };

    auto const layout = projectTerminalTrackColumnLayout(presentation, stored, 80);

    REQUIRE(layout.columns.size() == 3);
    CHECK(layout.columns[0] == TerminalTrackColumn{.field = rt::TrackField::Duration, .columns = 13});
    CHECK(layout.columns[1].field == rt::TrackField::Artist);
    CHECK(layout.columns[2].field == rt::TrackField::Title);
    CHECK(std::ranges::none_of(
      layout.columns, [](TerminalTrackColumn const& column) { return column.field == rt::TrackField::Album; }));
    auto const contentColumns = std::accumulate(layout.columns.begin(),
                                                layout.columns.end(),
                                                std::int32_t{0},
                                                [](std::int32_t const total, TerminalTrackColumn const& column)
                                                { return total + column.columns; });
    CHECK(contentColumns + trackTableChromeColumns(layout.columns.size()) == layout.availableColumns);
    CHECK(trackTableChromeColumns(0) == 3);
    CHECK(trackTableChromeColumns(1) == 6);
    CHECK(trackTableChromeColumns(3) == 10);
  }

  TEST_CASE("TerminalTrackColumnLayout - bounds restored fixed widths in terminal cells", "[tui][unit][track-columns]")
  {
    auto const presentation = rt::TrackPresentationSpec{
      .id = "terminal",
      .visibleFields = {rt::TrackField::Year, rt::TrackField::Duration},
    };
    auto const stored = std::vector{
      uimodel::TrackColumnState{.field = rt::TrackField::Year, .width = 1},
      uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 1000},
    };

    auto const layout = projectTerminalTrackColumnLayout(presentation, stored, 300);

    REQUIRE(layout.columns.size() == 2);
    CHECK(layout.columns[0].columns == kMinimumTrackColumnWidthColumns);
    CHECK(layout.columns[1].columns == kMaximumTrackColumnWidthColumns);
  }

  TEST_CASE("TerminalTrackColumnLayout - permits every presentation field to be hidden", "[tui][unit][track-columns]")
  {
    auto const presentation = rt::TrackPresentationSpec{
      .id = "hidden",
      .visibleFields = {rt::TrackField::Title, rt::TrackField::Artist},
    };
    auto const stored = std::vector{
      uimodel::TrackColumnState{.field = rt::TrackField::Title, .visible = false},
      uimodel::TrackColumnState{.field = rt::TrackField::Artist, .visible = false},
    };

    CHECK(projectTerminalTrackColumnLayout(presentation, stored, 80).columns.empty());
  }

  TEST_CASE("TerminalTrackColumnLayout - canonical resize survives viewport reflow", "[tui][unit][track-columns]")
  {
    auto const presentation = rt::TrackPresentationSpec{
      .id = "terminal",
      .visibleFields = {rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Duration},
    };
    auto const stored = std::vector{
      uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 11},
    };
    auto const before = projectTerminalTrackColumnLayout(presentation, stored, 100);
    auto const titleIt = std::ranges::find(before.columns, rt::TrackField::Title, &TerminalTrackColumn::field);
    REQUIRE(titleIt != before.columns.end());
    auto const target = titleIt->columns + 7;

    auto const resized = resizeTerminalTrackColumnLayout(presentation, stored, rt::TrackField::Title, target, 100);
    auto const sameViewport = projectTerminalTrackColumnLayout(presentation, resized, 100);
    auto const wideViewport = projectTerminalTrackColumnLayout(presentation, resized, 130);
    auto const resizedTitle =
      std::ranges::find(sameViewport.columns, rt::TrackField::Title, &TerminalTrackColumn::field);
    auto const wideTitle = std::ranges::find(wideViewport.columns, rt::TrackField::Title, &TerminalTrackColumn::field);
    auto const resizedDuration =
      std::ranges::find(sameViewport.columns, rt::TrackField::Duration, &TerminalTrackColumn::field);
    auto const wideDuration =
      std::ranges::find(wideViewport.columns, rt::TrackField::Duration, &TerminalTrackColumn::field);

    REQUIRE(resizedTitle != sameViewport.columns.end());
    REQUIRE(wideTitle != wideViewport.columns.end());
    REQUIRE(resizedDuration != sameViewport.columns.end());
    REQUIRE(wideDuration != wideViewport.columns.end());
    CHECK(resizedTitle->columns == target);
    CHECK(wideTitle->columns > resizedTitle->columns);
    CHECK(resizedDuration->columns == 11);
    CHECK(wideDuration->columns == 11);

    auto const canonicalTitle = std::ranges::find(resized, rt::TrackField::Title, &uimodel::TrackColumnState::field);
    auto const canonicalDuration =
      std::ranges::find(resized, rt::TrackField::Duration, &uimodel::TrackColumnState::field);
    REQUIRE(canonicalTitle != resized.end());
    REQUIRE(canonicalDuration != resized.end());
    CHECK(canonicalTitle->width == -1);
    CHECK(canonicalTitle->weight > 0.0);
    CHECK(canonicalDuration->width == 11);
    CHECK(canonicalDuration->weight == -1.0);
  }

  TEST_CASE("TerminalTrackColumnLayout - clamps pointer resize in terminal cells", "[tui][unit][track-columns]")
  {
    auto const presentation = rt::TrackPresentationSpec{
      .id = "terminal",
      .visibleFields = {rt::TrackField::Title, rt::TrackField::Duration},
    };

    auto const minimum = resizeTerminalTrackColumnLayout(presentation, {}, rt::TrackField::Duration, -100, 100);
    auto const maximum = resizeTerminalTrackColumnLayout(presentation, {}, rt::TrackField::Duration, 1000, 200);
    auto const minimumProjection = projectTerminalTrackColumnLayout(presentation, minimum, 100);
    auto const maximumProjection = projectTerminalTrackColumnLayout(presentation, maximum, 200);
    auto const minimumDuration =
      std::ranges::find(minimumProjection.columns, rt::TrackField::Duration, &TerminalTrackColumn::field);
    auto const maximumDuration =
      std::ranges::find(maximumProjection.columns, rt::TrackField::Duration, &TerminalTrackColumn::field);

    REQUIRE(minimumDuration != minimumProjection.columns.end());
    REQUIRE(maximumDuration != maximumProjection.columns.end());
    CHECK(minimumDuration->columns == kMinimumTrackColumnWidthColumns);
    CHECK(maximumDuration->columns == kMaximumTrackColumnWidthColumns);
  }
} // namespace ao::tui::test
