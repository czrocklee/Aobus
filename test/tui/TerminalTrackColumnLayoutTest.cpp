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
  TEST_CASE("TerminalTrackColumnLayout - applies persisted order visibility and cell widths")
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
      uimodel::TrackColumnState{.field = rt::TrackField::Album, .visibility = uimodel::TrackColumnVisibility::Hide},
      uimodel::TrackColumnState{.field = rt::TrackField::Artist, .weight = 1.0},
    };

    auto const layout = projectTerminalTrackColumnLayout(presentation, stored, 82);

    REQUIRE(layout.columns.size() == 3);
    CHECK(layout.columns[0] == TerminalTrackColumn{.field = rt::TrackField::Duration, .columns = 13});
    CHECK(layout.columns[1] == TerminalTrackColumn{.field = rt::TrackField::Artist, .columns = 14});
    CHECK(layout.columns[2] == TerminalTrackColumn{.field = rt::TrackField::Title, .columns = 43});
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
    CHECK(trackTableChromeColumns(3) == 12);
  }

  TEST_CASE("TerminalTrackColumnLayout - projects a stored show for the suppressed Album column")
  {
    auto const* preset = rt::builtinTrackPresentationPreset("albums");
    REQUIRE(preset != nullptr);
    auto const stored = std::vector{uimodel::TrackColumnState{
      .field = rt::TrackField::Album, .weight = 1.75, .visibility = uimodel::TrackColumnVisibility::Show}};

    auto const layout = projectTerminalTrackColumnLayout(preset->spec, stored, 100);

    REQUIRE_FALSE(layout.columns.empty());
    CHECK(layout.columns.front().field == rt::TrackField::Album);
  }

  TEST_CASE("TerminalTrackColumnLayout - a no-op resize leaves visibility-only overrides without geometry")
  {
    auto const* preset = rt::builtinTrackPresentationPreset("albums");
    REQUIRE(preset != nullptr);
    auto const shownAlbum =
      uimodel::TrackColumnState{.field = rt::TrackField::Album, .visibility = uimodel::TrackColumnVisibility::Show};
    auto const hiddenArtist =
      uimodel::TrackColumnState{.field = rt::TrackField::Artist, .visibility = uimodel::TrackColumnVisibility::Hide};
    auto const stored = std::vector{shownAlbum, hiddenArtist};

    auto const layout = projectTerminalTrackColumnLayout(preset->spec, stored, 100);
    REQUIRE(layout.columns.size() == 4);
    CHECK(layout.columns.front().field == rt::TrackField::DisplayTrackNumber);
    CHECK(layout.columns.back().field == rt::TrackField::Album);
    CHECK_FALSE(std::ranges::contains(layout.columns, rt::TrackField::Artist, &TerminalTrackColumn::field));
    auto const duration = std::ranges::find(layout.columns, rt::TrackField::Duration, &TerminalTrackColumn::field);
    REQUIRE(duration != layout.columns.end());
    CHECK(resizeTerminalTrackColumnLayout(preset->spec, stored, rt::TrackField::Duration, duration->columns, 100) ==
          stored);
    CHECK(resizeTerminalTrackColumnLayout(preset->spec, stored, rt::TrackField::Artist, 20, 100) == stored);

    auto const resized = resizeTerminalTrackColumnLayout(preset->spec, stored, rt::TrackField::Duration, 20, 100);
    auto const album = std::ranges::find(resized, rt::TrackField::Album, &uimodel::TrackColumnState::field);
    REQUIRE(album != resized.end());
    CHECK(album->visibility == uimodel::TrackColumnVisibility::Show);
    CHECK(album->hasPosition());
    CHECK(std::ranges::contains(resized, hiddenArtist));
    auto const resizedLayout = projectTerminalTrackColumnLayout(preset->spec, resized, 100);
    CHECK(std::ranges::contains(
      resizedLayout.columns, TerminalTrackColumn{.field = rt::TrackField::Duration, .columns = 20}));
  }

  TEST_CASE("TerminalTrackColumnLayout - bounds restored fixed widths in terminal cells")
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

  TEST_CASE("TerminalTrackColumnLayout - permits every presentation field to be hidden")
  {
    auto const presentation = rt::TrackPresentationSpec{
      .id = "hidden",
      .visibleFields = {rt::TrackField::Title, rt::TrackField::Duration},
    };
    auto const stored = std::vector{
      uimodel::TrackColumnState{
        .field = rt::TrackField::Title, .weight = 2.5, .visibility = uimodel::TrackColumnVisibility::Hide},
      uimodel::TrackColumnState{
        .field = rt::TrackField::Duration, .width = 12, .visibility = uimodel::TrackColumnVisibility::Hide},
    };

    CHECK(projectTerminalTrackColumnLayout(presentation, stored, 80).columns.empty());
    CHECK(resizeTerminalTrackColumnLayout(presentation, stored, rt::TrackField::Duration, 15, 80) == stored);
  }

  TEST_CASE("TerminalTrackColumnLayout - canonical resize survives viewport reflow")
  {
    auto const presentation = rt::TrackPresentationSpec{
      .id = "terminal",
      .visibleFields = {rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Album, rt::TrackField::Duration},
    };
    auto const hiddenAlbum = uimodel::TrackColumnState{
      .field = rt::TrackField::Album, .weight = 1.75, .visibility = uimodel::TrackColumnVisibility::Hide};
    auto const stored = std::vector{
      uimodel::TrackColumnState{.field = rt::TrackField::Duration, .width = 11},
      hiddenAlbum,
      uimodel::TrackColumnState{.field = rt::TrackField::Title, .weight = 3.0},
    };
    auto const before = projectTerminalTrackColumnLayout(presentation, stored, 102);
    auto const titleIt = std::ranges::find(before.columns, rt::TrackField::Title, &TerminalTrackColumn::field);
    REQUIRE(titleIt != before.columns.end());
    // At 102 terminal cells, twelve chrome cells and the fixed 11-cell duration
    // leave 79 cells: Title 47, Artist 32. Request a literal 55-cell title.
    CHECK(titleIt->columns == 47);
    auto const artistIt = std::ranges::find(before.columns, rt::TrackField::Artist, &TerminalTrackColumn::field);
    REQUIRE(artistIt != before.columns.end());
    CHECK(artistIt->columns == 32);
    constexpr int kTargetColumns = 55;

    auto const resized =
      resizeTerminalTrackColumnLayout(presentation, stored, rt::TrackField::Title, kTargetColumns, 102);
    REQUIRE(resized.size() == 4);
    CHECK(resized[1] == hiddenAlbum);
    auto const sameViewport = projectTerminalTrackColumnLayout(presentation, resized, 102);
    auto const wideViewport = projectTerminalTrackColumnLayout(presentation, resized, 132);
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
    CHECK(resizedTitle->columns == 55);
    CHECK(wideTitle->columns == 76);
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

  TEST_CASE("TerminalTrackColumnLayout - clamps pointer resize in terminal cells")
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
