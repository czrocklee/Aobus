// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include <app/linux-gtk/track/TrackPresentation.h>

namespace ao::gtk::test
{
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
      .columns = {{.column = TrackColumn::Title, .visible = true, .width = 300}},
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
    using namespace ao::rt;

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

  TEST_CASE("TrackPresentation - editable trait", "[app][presentation]")
  {
    for (auto const& def : trackColumnDefinitions())
    {
      bool const expected =
        def.column == TrackColumn::Title || def.column == TrackColumn::Artist || def.column == TrackColumn::Album;
      CHECK(def.editable == expected);
    }
  }

  TEST_CASE("TrackPresentation - trackColumnForPresentationField mapping", "[app][presentation]")
  {
    using namespace ao::rt;

    CHECK(trackColumnForPresentationField(TrackPresentationField::Title) == TrackColumn::Title);
    CHECK(trackColumnForPresentationField(TrackPresentationField::Artist) == TrackColumn::Artist);
    CHECK(trackColumnForPresentationField(TrackPresentationField::Album) == TrackColumn::Album);
    CHECK(trackColumnForPresentationField(TrackPresentationField::AlbumArtist) == TrackColumn::AlbumArtist);
    CHECK(trackColumnForPresentationField(TrackPresentationField::Genre) == TrackColumn::Genre);
    CHECK(trackColumnForPresentationField(TrackPresentationField::Composer) == TrackColumn::Composer);
    CHECK(trackColumnForPresentationField(TrackPresentationField::Work) == TrackColumn::Work);
    CHECK(trackColumnForPresentationField(TrackPresentationField::Year) == TrackColumn::Year);
    CHECK(trackColumnForPresentationField(TrackPresentationField::DiscNumber) == TrackColumn::DiscNumber);
    CHECK(trackColumnForPresentationField(TrackPresentationField::TrackNumber) == TrackColumn::TrackNumber);
    CHECK(trackColumnForPresentationField(TrackPresentationField::Duration) == TrackColumn::Duration);
    CHECK(trackColumnForPresentationField(TrackPresentationField::Tags) == TrackColumn::Tags);
  }

  TEST_CASE("TrackPresentation - draggable trait", "[app][presentation]")
  {
    for (auto const& def : trackColumnDefinitions())
    {
      bool const expected =
        def.column == TrackColumn::Artist || def.column == TrackColumn::Album || def.column == TrackColumn::Genre;
      CHECK(def.draggable == expected);
    }
  }
} // namespace ao::gtk::test
