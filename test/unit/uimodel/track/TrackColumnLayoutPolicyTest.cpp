// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/uimodel/track/TrackColumnLayoutPolicy.h>
#include <ao/uimodel/track/TrackFieldPresentationPolicy.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::uimodel::track::test
{
  TEST_CASE("TrackColumnLayoutPolicy - chooses the expanding column from visible fields",
            "[uimodel][unit][track][presentation]")
  {
    auto fields = std::vector{rt::TrackField::Artist, rt::TrackField::Title};
    CHECK(expandingTrackColumn(fields) == rt::TrackField::Title);

    fields = {rt::TrackField::Artist, rt::TrackField::Tags};
    CHECK(expandingTrackColumn(fields) == rt::TrackField::Tags);

    fields = {rt::TrackField::Album, rt::TrackField::Artist};
    CHECK(expandingTrackColumn(fields) == rt::TrackField::Album);

    fields = {};
    CHECK(expandingTrackColumn(fields) == rt::TrackField::Title);
  }

  TEST_CASE("TrackColumnLayoutPolicy - orders visible fields using stored order first",
            "[uimodel][unit][track][presentation]")
  {
    auto const visible = std::vector{rt::TrackField::Title, rt::TrackField::Artist, rt::TrackField::Album};
    auto const stored = std::vector{rt::TrackField::Album, rt::TrackField::DiscNumber, rt::TrackField::Title};

    auto const ordered = visibleTrackFieldsInStoredOrder(visible, stored);

    REQUIRE(ordered.size() == 3);
    CHECK(ordered[0] == rt::TrackField::Album);
    CHECK(ordered[1] == rt::TrackField::Title);
    CHECK(ordered[2] == rt::TrackField::Artist);
  }

  TEST_CASE("TrackColumnLayoutPolicy - uses stored widths only when positive", "[uimodel][unit][track][presentation]")
  {
    CHECK(effectiveTrackFieldColumnWidth(rt::TrackField::Title, 321) == 321);
    CHECK(effectiveTrackFieldColumnWidth(rt::TrackField::Title, 0) ==
          defaultTrackFieldColumnWidth(rt::TrackField::Title));
    CHECK(effectiveTrackFieldColumnWidth(rt::TrackField::Title, -1) ==
          defaultTrackFieldColumnWidth(rt::TrackField::Title));
  }
} // namespace ao::uimodel::track::test
