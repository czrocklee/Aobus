// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <app/TrackPresentation.h>

namespace
{
  auto makeKeys(rs::core::TrackId id,
                std::string_view artist,
                std::string_view album,
                std::string_view albumArtist,
                std::string_view genre,
                std::string_view title,
                std::uint16_t year,
                std::uint16_t discNumber,
                std::uint16_t trackNumber) -> TrackPresentationKeysView
  {
    return TrackPresentationKeysView{
      .artist = artist,
      .album = album,
      .albumArtist = albumArtist,
      .genre = genre,
      .title = title,
      .year = year,
      .discNumber = discNumber,
      .trackNumber = trackNumber,
      .trackId = id,
    };
  }
}

TEST_CASE("TrackPresentation presets match grouped songs mode", "[app][presentation]")
{
  auto const artistSpec = presentationSpecForGroup(TrackGroupBy::Artist);

  REQUIRE(artistSpec.groupBy == TrackGroupBy::Artist);
  REQUIRE(artistSpec.sortBy == std::vector<TrackSortTerm>{{TrackSortField::Artist},
                                                          {TrackSortField::Album},
                                                          {TrackSortField::DiscNumber},
                                                          {TrackSortField::TrackNumber},
                                                          {TrackSortField::Title}});

  auto const noneSpec = presentationSpecForGroup(TrackGroupBy::None);
  REQUIRE(noneSpec.sortBy.empty());

  REQUIRE(shouldShowColumn(TrackGroupBy::None, TrackColumn::Artist));
  REQUIRE(shouldShowColumn(TrackGroupBy::None, TrackColumn::Album));
  REQUIRE(shouldShowColumn(TrackGroupBy::None, TrackColumn::DiscNumber));
  REQUIRE(shouldShowColumn(TrackGroupBy::None, TrackColumn::TrackNumber));

  REQUIRE_FALSE(shouldShowColumn(TrackGroupBy::Artist, TrackColumn::Artist));
  REQUIRE(shouldShowColumn(TrackGroupBy::Artist, TrackColumn::Album));

  REQUIRE_FALSE(shouldShowColumn(TrackGroupBy::Album, TrackColumn::Artist));
  REQUIRE_FALSE(shouldShowColumn(TrackGroupBy::Album, TrackColumn::Album));
  REQUIRE(shouldShowColumn(TrackGroupBy::Album, TrackColumn::DiscNumber));
  REQUIRE(shouldShowColumn(TrackGroupBy::Album, TrackColumn::TrackNumber));
}

TEST_CASE("TrackPresentation sorts grouped rows deterministically", "[app][presentation]")
{
  auto const spec = presentationSpecForGroup(TrackGroupBy::Artist);
  auto const known = makeKeys(rs::core::TrackId{2}, "Beatles", "Abbey Road", "", "Rock", "Something", 1969, 1, 2);
  auto const unknown = makeKeys(rs::core::TrackId{1}, "", "Loose Ends", "", "", "Untitled", 0, 0, 0);
  auto const tieA = makeKeys(rs::core::TrackId{3}, "Beatles", "Abbey Road", "", "Rock", "Something", 1969, 1, 2);

  REQUIRE(compareForSort(known, unknown, spec.sortBy) < 0);
  REQUIRE(compareForSort(tieA, known, spec.sortBy) > 0);
}

TEST_CASE("TrackPresentation keeps album groups split by album artist", "[app][presentation]")
{
  auto const first =
    makeKeys(rs::core::TrackId{10}, "Artist A", "Greatest Hits", "Artist A", "Rock", "One", 2000, 1, 1);
  auto const second =
    makeKeys(rs::core::TrackId{11}, "Artist B", "Greatest Hits", "Artist B", "Rock", "Two", 2001, 1, 1);

  REQUIRE(compareForGrouping(first, second, TrackGroupBy::Album) < 0);
  REQUIRE(groupLabelFor(first, TrackGroupBy::Album) == "Greatest Hits - Artist A");
  REQUIRE(groupLabelFor(makeKeys(rs::core::TrackId{12}, "", "", "", "", "", 0, 0, 0), TrackGroupBy::Year) ==
          "Unknown Year");
}
