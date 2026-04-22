// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include "platform/linux/ui/TrackPresentation.h"

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
                std::uint16_t trackNumber) -> app::ui::TrackPresentationKeysView
  {
    return app::ui::TrackPresentationKeysView{
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

TEST_CASE("app::ui::TrackPresentation presets match grouped songs mode", "[app][presentation]")
{
  using namespace app::ui;

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
}
