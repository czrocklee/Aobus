// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackFieldPresentationPolicy.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("defaultTrackFieldColumnWidth returns presentation widths by field role",
            "[uimodel][unit][library][presentation]")
  {
    CHECK(defaultTrackFieldColumnWidth(rt::TrackField::Artist) == 150);
    CHECK(defaultTrackFieldColumnWidth(rt::TrackField::Album) == 200);
    CHECK(defaultTrackFieldColumnWidth(rt::TrackField::TrackNumber) == 72);
    CHECK(defaultTrackFieldColumnWidth(rt::TrackField::Duration) == 84);
    CHECK(defaultTrackFieldColumnWidth(rt::TrackField::Year) == 80);
    CHECK(defaultTrackFieldColumnWidth(rt::TrackField::AlbumArtist) == 180);
    CHECK(defaultTrackFieldColumnWidth(rt::TrackField::TechnicalSummary) == 180);
    CHECK(defaultTrackFieldColumnWidth(rt::TrackField::FilePath) == 300);
  }

  TEST_CASE("trackFieldIsVisibleByDefault follows the default presentation visible fields",
            "[uimodel][unit][library][presentation]")
  {
    CHECK(trackFieldIsVisibleByDefault(rt::TrackField::Title));
    CHECK(trackFieldIsVisibleByDefault(rt::TrackField::Artist));
    CHECK(trackFieldIsVisibleByDefault(rt::TrackField::Album));
    CHECK(trackFieldIsVisibleByDefault(rt::TrackField::Year));
    CHECK(trackFieldIsVisibleByDefault(rt::TrackField::Duration));
    CHECK(trackFieldIsVisibleByDefault(rt::TrackField::Tags));
    CHECK(trackFieldIsVisibleByDefault(rt::TrackField::DisplayTrackNumber));

    CHECK_FALSE(trackFieldIsVisibleByDefault(rt::TrackField::AlbumArtist));
    CHECK_FALSE(trackFieldIsVisibleByDefault(rt::TrackField::Genre));
    CHECK_FALSE(trackFieldIsVisibleByDefault(rt::TrackField::TrackNumber));
  }

  TEST_CASE("trackFieldColumnTitle returns runtime field labels", "[uimodel][unit][library][presentation]")
  {
    CHECK(trackFieldColumnTitle(rt::TrackField::Title) == "Title");
    CHECK(trackFieldColumnTitle(rt::TrackField::Artist) == "Artist");
    CHECK(trackFieldColumnTitle(rt::TrackField::Duration) == "Duration");
  }

  TEST_CASE("redundantSortFieldColumn maps groupable sort fields to presentation columns",
            "[uimodel][unit][library][presentation]")
  {
    CHECK(redundantSortFieldColumn(rt::TrackSortField::Artist) == rt::TrackField::Artist);
    CHECK(redundantSortFieldColumn(rt::TrackSortField::Album) == rt::TrackField::Album);
    CHECK(redundantSortFieldColumn(rt::TrackSortField::AlbumArtist) == rt::TrackField::AlbumArtist);
    CHECK(redundantSortFieldColumn(rt::TrackSortField::Genre) == rt::TrackField::Genre);
    CHECK(redundantSortFieldColumn(rt::TrackSortField::Composer) == rt::TrackField::Composer);
    CHECK(redundantSortFieldColumn(rt::TrackSortField::Work) == rt::TrackField::Work);
    CHECK(redundantSortFieldColumn(rt::TrackSortField::Year) == rt::TrackField::Year);

    CHECK_FALSE(redundantSortFieldColumn(rt::TrackSortField::Title).has_value());
    CHECK_FALSE(redundantSortFieldColumn(rt::TrackSortField::Duration).has_value());
    CHECK_FALSE(redundantSortFieldColumn(rt::TrackSortField::DiscNumber).has_value());
    CHECK_FALSE(redundantSortFieldColumn(rt::TrackSortField::TrackNumber).has_value());
  }

  TEST_CASE("presentable runtime fields have default presentation policy", "[uimodel][unit][library][presentation]")
  {
    for (auto const& rtDef : rt::trackFieldDefinitions())
    {
      if (!rtDef.presentable)
      {
        continue;
      }

      INFO("Field " << rtDef.id << " must have a column title and width");
      CHECK_FALSE(trackFieldColumnTitle(rtDef.field).empty());
      CHECK(defaultTrackFieldColumnWidth(rtDef.field) > 0);
    }
  }
} // namespace ao::uimodel::test
