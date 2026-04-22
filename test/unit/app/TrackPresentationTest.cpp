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
}
