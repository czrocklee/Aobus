// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/presentation/TrackColumnLayoutPolicy.h>

#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("TrackColumnLayoutPolicy - orders visible fields using stored order first",
            "[uimodel][unit][library][presentation]")
  {
    auto const visible = std::vector{rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Album};
    auto const stored = std::vector{rt::TrackField::Album, rt::TrackField::DiscNumber, rt::TrackField::Title};

    auto const ordered = visibleTrackFieldsInStoredOrder(visible, stored);

    REQUIRE(ordered.size() == 3);
    CHECK(ordered[0] == rt::TrackField::Album);
    CHECK(ordered[1] == rt::TrackField::Title);
    CHECK(ordered[2] == rt::TrackField::Artist);
  }

  TEST_CASE("TrackColumnLayoutPolicy - stored visibility filters presentation fields without hiding new fields",
            "[uimodel][unit][library][presentation]")
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

  TEST_CASE("TrackColumnLayoutPolicy - visible updates preserve hidden column slots",
            "[uimodel][unit][library][presentation]")
  {
    auto const stored = std::vector{
      TrackColumnState{.field = rt::TrackField::Title, .weight = 1.0},
      TrackColumnState{.field = rt::TrackField::Genre, .weight = 1.0, .visible = false},
      TrackColumnState{.field = rt::TrackField::Artist, .weight = 1.0},
      TrackColumnState{.field = rt::TrackField::Album, .weight = 1.0, .visible = false},
    };
    auto const updated = std::vector{
      TrackColumnState{.field = rt::TrackField::Artist, .weight = 2.0},
      TrackColumnState{.field = rt::TrackField::Title, .weight = 3.0},
      TrackColumnState{.field = rt::TrackField::Duration, .width = 95},
    };

    auto const merged = mergeVisibleTrackColumnLayout(stored, updated);

    REQUIRE(merged.size() == 5);
    CHECK(merged[0] == updated[0]);
    CHECK(merged[1] == stored[1]);
    CHECK(merged[2] == updated[1]);
    CHECK(merged[3] == stored[3]);
    CHECK(merged[4] == updated[2]);
  }
} // namespace ao::uimodel::test
