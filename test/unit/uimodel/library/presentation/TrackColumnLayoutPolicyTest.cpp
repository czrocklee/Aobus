// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutPolicy.h>

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
} // namespace ao::uimodel::test
