// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/library/presentation/TrackColumnWidthSolver.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("TrackColumnLayouts - orders visible fields using stored order first")
  {
    auto const visible = std::vector{rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Album};
    auto const stored = std::vector{rt::TrackField::Album, rt::TrackField::DiscNumber, rt::TrackField::Title};

    auto const ordered = visibleTrackFieldsInStoredOrder(visible, stored);

    REQUIRE(ordered.size() == 3);
    CHECK(ordered[0] == rt::TrackField::Album);
    CHECK(ordered[1] == rt::TrackField::Title);
    CHECK(ordered[2] == rt::TrackField::Artist);
  }

  TEST_CASE("TrackColumnLayouts - stored visibility filters presentation fields without hiding new fields")
  {
    auto const presentation = std::vector{rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Album};
    auto const stored = std::vector{
      TrackColumnState{.field = rt::TrackField::Album, .weight = 1.0, .visible = false},
      TrackColumnState{.field = rt::TrackField::Artist, .weight = 1.0},
    };

    auto const visible = visibleTrackFieldsInStoredLayout(presentation, stored);

    REQUIRE(visible.size() == 2);
    CHECK(visible[0] == rt::TrackField::Artist);
    CHECK(visible[1] == rt::TrackField::Title);
  }

  TEST_CASE("TrackColumnLayouts - canonical visible updates preserve hidden and unavailable column slots")
  {
    auto const hiddenGenre = TrackColumnState{.field = rt::TrackField::Genre, .weight = 1.25, .visible = false};
    auto const unavailableAlbum = TrackColumnState{.field = rt::TrackField::Album, .weight = 1.5};
    auto const stored = std::vector{
      TrackColumnState{.field = rt::TrackField::Title, .weight = 1.0},
      hiddenGenre,
      TrackColumnState{.field = rt::TrackField::Artist, .weight = 1.0},
      unavailableAlbum,
    };
    auto const visibleSpecs = std::vector{
      TrackColumnSolveSpec{.field = rt::TrackField::Artist, .weight = 2.1254},
      TrackColumnSolveSpec{.field = rt::TrackField::Title},
      TrackColumnSolveSpec{.field = rt::TrackField::Duration, .fixedWidth = 95},
    };

    auto const merged = mergeVisibleTrackColumnSpecs(stored, visibleSpecs);

    REQUIRE(merged.size() == 5);
    CHECK(merged[0] == TrackColumnState{.field = rt::TrackField::Artist, .weight = 2.125});
    CHECK(merged[1] == hiddenGenre);
    CHECK(merged[2] == TrackColumnState{.field = rt::TrackField::Title, .weight = 3.0});
    CHECK(merged[3] == unavailableAlbum);
    CHECK(merged[4] == TrackColumnState{.field = rt::TrackField::Duration, .width = 95});
  }
} // namespace ao::uimodel::test
