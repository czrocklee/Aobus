// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include "platform/linux/ui/TrackPresentation.h"

namespace
{
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
  REQUIRE(shouldShowColumn(TrackGroupBy::None, TrackColumn::Duration));

  auto const defaultLayout = defaultTrackColumnLayout();
  REQUIRE(defaultLayout.columns.size() == trackColumnDefinitions().size());
  REQUIRE(defaultLayout.columns.at(4) == TrackColumnState{TrackColumn::Duration, true, 84});

  REQUIRE_FALSE(shouldShowColumn(TrackGroupBy::AlbumArtist, TrackColumn::AlbumArtist));
  REQUIRE_FALSE(shouldShowColumn(TrackGroupBy::Genre, TrackColumn::Genre));
  REQUIRE_FALSE(shouldShowColumn(TrackGroupBy::Year, TrackColumn::Year));
}

TEST_CASE("app::ui::TrackPresentation duration sorting", "[app][presentation]")
{
  using namespace app::ui;

  auto const sortBy = std::vector<TrackSortTerm>{{TrackSortField::Duration}};

  TrackPresentationKeysView shortTrack{.durationMs = 1000, .trackId = rs::core::TrackId{1}};
  TrackPresentationKeysView longTrack{.durationMs = 5000, .trackId = rs::core::TrackId{2}};
  TrackPresentationKeysView noDurationTrack{.durationMs = 0, .trackId = rs::core::TrackId{3}};

  SECTION("shorter duration comes first")
  {
    REQUIRE(compareForSort(shortTrack, longTrack, sortBy) < 0);
    REQUIRE(compareForSort(longTrack, shortTrack, sortBy) > 0);
  }

  SECTION("unknown duration (0) comes last")
  {
    REQUIRE(compareForSort(shortTrack, noDurationTrack, sortBy) < 0);
    REQUIRE(compareForSort(noDurationTrack, shortTrack, sortBy) > 0);
  }

  SECTION("equal duration falls back to track ID")
  {
    TrackPresentationKeysView shortTrack2{.durationMs = 1000, .trackId = rs::core::TrackId{4}};
    REQUIRE(compareForSort(shortTrack, shortTrack2, sortBy) < 0);
  }
}
