// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "TrackPresentation.h"

using namespace ao::gtk;

TEST_CASE("TrackPresentation - column definitions", "[app][presentation]")
{
  auto const definitions = trackColumnDefinitions();
  REQUIRE(definitions.size() == 12);
}

TEST_CASE("TrackPresentation - default column layout", "[app][presentation]")
{
  auto const layout = defaultTrackColumnLayout();
  REQUIRE(layout.columns.size() == trackColumnDefinitions().size());

  auto const duration = std::ranges::find(layout.columns, TrackColumn::Duration, &TrackColumnState::column);
  REQUIRE(duration != layout.columns.end());
  CHECK(duration->visible == true);
  CHECK(duration->width == 84);
}

TEST_CASE("TrackPresentation - normalize column layout fills missing", "[app][presentation]")
{
  auto layout = TrackColumnLayout{
    .columns = {{TrackColumn::Title, true, 300}},
  };
  auto const normalized = normalizeTrackColumnLayout(layout);

  REQUIRE(normalized.columns.size() == trackColumnDefinitions().size());
  auto const title = std::ranges::find(normalized.columns, TrackColumn::Title, &TrackColumnState::column);
  REQUIRE(title != normalized.columns.end());
  CHECK(title->visible == true);
  CHECK(title->width == 300);
}

TEST_CASE("TrackPresentation - redundantFieldToColumn mapping", "[app][presentation]")
{
  using ao::rt::TrackSortField;

  CHECK(redundantFieldToColumn(TrackSortField::Artist) == TrackColumn::Artist);
  CHECK(redundantFieldToColumn(TrackSortField::Album) == TrackColumn::Album);
  CHECK(redundantFieldToColumn(TrackSortField::AlbumArtist) == TrackColumn::AlbumArtist);
  CHECK(redundantFieldToColumn(TrackSortField::Genre) == TrackColumn::Genre);
  CHECK(redundantFieldToColumn(TrackSortField::Composer) == TrackColumn::Composer);
  CHECK(redundantFieldToColumn(TrackSortField::Work) == TrackColumn::Work);
  CHECK(redundantFieldToColumn(TrackSortField::Year) == TrackColumn::Year);

  // Non-display fields should return nullopt
  CHECK_FALSE(redundantFieldToColumn(TrackSortField::Title).has_value());
  CHECK_FALSE(redundantFieldToColumn(TrackSortField::Duration).has_value());
  CHECK_FALSE(redundantFieldToColumn(TrackSortField::DiscNumber).has_value());
  CHECK_FALSE(redundantFieldToColumn(TrackSortField::TrackNumber).has_value());
}
