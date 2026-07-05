// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/TrackField.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutStore.h>
#include <ao/uimodel/library/presentation/TrackColumnWidthSolver.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    TrackColumnSolveSpec flexible(rt::TrackField field,
                                  double weight = 1.0,
                                  std::int32_t defaultWidth = 100,
                                  std::int32_t minimumWidth = 20)
    {
      return TrackColumnSolveSpec{
        .field = field, .weight = weight, .fixedWidth = -1, .defaultWidth = defaultWidth, .minimumWidth = minimumWidth};
    }

    TrackColumnSolveSpec fixed(rt::TrackField field, std::int32_t defaultWidth, std::int32_t minimumWidth = 40)
    {
      return TrackColumnSolveSpec{
        .field = field, .weight = -1.0, .fixedWidth = -1, .defaultWidth = defaultWidth, .minimumWidth = minimumWidth};
    }

    std::int32_t totalWidth(std::vector<std::int32_t> const& widths)
    {
      return std::accumulate(widths.begin(), widths.end(), std::int32_t{0});
    }
  } // namespace

  TEST_CASE("TrackColumnWidthSolver - distributes flexible columns by weight", "[uimodel][unit][library][presentation]")
  {
    auto const specs = std::vector{
      fixed(rt::TrackField::Duration, 80),
      flexible(rt::TrackField::Title, 3.0),
      flexible(rt::TrackField::Artist, 1.0),
    };

    auto const widths = solveTrackColumnWidths(specs, 480);

    REQUIRE(widths.size() == 3);
    CHECK(widths[0] == 80);
    CHECK(widths[1] == 300);
    CHECK(widths[2] == 100);
    CHECK(totalWidth(widths) == 480);
  }

  TEST_CASE("TrackColumnWidthSolver - pins minimum columns and redistributes remaining width",
            "[uimodel][unit][library][presentation]")
  {
    auto const specs = std::vector{
      fixed(rt::TrackField::Duration, 50),
      flexible(rt::TrackField::Title, 1.0, 100, 80),
      flexible(rt::TrackField::Artist, 1.0, 100, 80),
      flexible(rt::TrackField::Album, 10.0, 100, 20),
    };

    auto const widths = solveTrackColumnWidths(specs, 330);

    REQUIRE(widths.size() == 4);
    CHECK(widths[0] == 50);
    CHECK(widths[1] == 80);
    CHECK(widths[2] == 80);
    CHECK(widths[3] == 120);
    CHECK(totalWidth(widths) == 330);
  }

  TEST_CASE("TrackColumnWidthSolver - returns minimum flexible widths when the viewport overflows",
            "[uimodel][unit][library][presentation]")
  {
    auto const specs = std::vector{
      fixed(rt::TrackField::Duration, 100),
      flexible(rt::TrackField::Title, 1.0, 100, 80),
      flexible(rt::TrackField::Artist, 1.0, 100, 80),
    };

    auto const widths = solveTrackColumnWidths(specs, 200);

    REQUIRE(widths.size() == 3);
    CHECK(widths[0] == 100);
    CHECK(widths[1] == 80);
    CHECK(widths[2] == 80);
    CHECK(totalWidth(widths) == 260);
  }

  TEST_CASE("TrackColumnWidthSolver - leaves space when every visible column is fixed",
            "[uimodel][unit][library][presentation]")
  {
    auto const specs = std::vector{
      fixed(rt::TrackField::Duration, 80),
      fixed(rt::TrackField::Year, 60),
    };

    auto const widths = solveTrackColumnWidths(specs, 400);

    REQUIRE(widths.size() == 2);
    CHECK(widths[0] == 80);
    CHECK(widths[1] == 60);
    CHECK(totalWidth(widths) == 140);
  }

  TEST_CASE("TrackColumnWidthSolver - derives stable weights from solved widths",
            "[uimodel][unit][library][presentation]")
  {
    auto const specs = std::vector{
      fixed(rt::TrackField::Duration, 80),
      flexible(rt::TrackField::Title, 3.0),
      flexible(rt::TrackField::Artist, 1.0),
    };
    auto const widths = solveTrackColumnWidths(specs, 480);

    auto const resizedSpecs = specsFromWidths(specs, widths);
    auto const secondWidths = solveTrackColumnWidths(resizedSpecs, 480);

    REQUIRE(resizedSpecs.size() == 3);
    CHECK(resizedSpecs[1].weight == 1.5);
    CHECK(resizedSpecs[2].weight == 0.5);
    CHECK(secondWidths == widths);
  }

  TEST_CASE("TrackColumnWidthSolver - round-trips coarse non-dividing widths by sum and convergence",
            "[uimodel][unit][library][presentation]")
  {
    // A coarse viewport that does not divide evenly across the weights exposes the
    // limit of weight-based round-tripping: re-deriving weights from solved widths
    // and re-solving does not reproduce the widths exactly. The contract the frontends
    // rely on is weaker but still stable: the total is preserved on every pass, drift
    // stays within one unit per column, and a second round-trip reaches a fixed point.
    constexpr std::int32_t kViewport = 7;
    auto const specs = std::vector{
      flexible(rt::TrackField::Title, 1.0, 100, 1),
      flexible(rt::TrackField::Artist, 1.0, 100, 1),
      flexible(rt::TrackField::Album, 1.0, 100, 1),
    };

    auto const firstWidths = solveTrackColumnWidths(specs, kViewport);
    REQUIRE(firstWidths.size() == 3);
    CHECK(totalWidth(firstWidths) == kViewport);

    auto const secondSpecs = specsFromWidths(specs, firstWidths);
    auto const secondWidths = solveTrackColumnWidths(secondSpecs, kViewport);
    REQUIRE(secondWidths.size() == 3);
    CHECK(totalWidth(secondWidths) == kViewport);

    for (std::size_t index = 0; index < firstWidths.size(); ++index)
    {
      CHECK(std::abs(secondWidths[index] - firstWidths[index]) <= 1);
    }

    auto const thirdSpecs = specsFromWidths(secondSpecs, secondWidths);
    auto const thirdWidths = solveTrackColumnWidths(thirdSpecs, kViewport);
    CHECK(thirdWidths == secondWidths);
  }

  TEST_CASE("TrackColumnWidthSolver - resizes a flexible column by absorbing width on the right",
            "[uimodel][unit][library][presentation]")
  {
    auto const specs = std::vector{
      flexible(rt::TrackField::Title),
      flexible(rt::TrackField::Artist),
      flexible(rt::TrackField::Album),
    };

    auto const resized = resizeTrackColumnSpecs(specs, rt::TrackField::Title, 300, 600);
    auto const widths = solveTrackColumnWidths(resized, 600);

    REQUIRE(widths.size() == 3);
    CHECK(widths[0] == 300);
    CHECK(widths[1] == 150);
    CHECK(widths[2] == 150);
    CHECK(totalWidth(widths) == 600);
  }

  TEST_CASE("TrackColumnWidthSolver - moves to left flexible columns only after right columns hit minimum",
            "[uimodel][unit][library][presentation]")
  {
    auto const specs = std::vector{
      flexible(rt::TrackField::Artist, 1.0, 100, 80),
      flexible(rt::TrackField::Title, 1.0, 100, 80),
      flexible(rt::TrackField::Album, 1.0, 100, 180),
    };

    auto const resized = resizeTrackColumnSpecs(specs, rt::TrackField::Title, 300, 600);
    auto const widths = solveTrackColumnWidths(resized, 600);

    REQUIRE(widths.size() == 3);
    CHECK(widths[0] == 120);
    CHECK(widths[1] == 300);
    CHECK(widths[2] == 180);
    CHECK(totalWidth(widths) == 600);
  }

  TEST_CASE("TrackColumnWidthSolver - lets fixed resizing create overflow instead of rebounding",
            "[uimodel][unit][library][presentation]")
  {
    auto const specs = std::vector{
      fixed(rt::TrackField::Duration, 80, 40),
      flexible(rt::TrackField::Title, 1.0, 100, 80),
    };

    auto const resized = resizeTrackColumnSpecs(specs, rt::TrackField::Duration, 560, 600);
    auto const widths = solveTrackColumnWidths(resized, 600);

    REQUIRE(widths.size() == 2);
    CHECK(widths[0] == 560);
    CHECK(widths[1] == 80);
    CHECK(totalWidth(widths) == 640);
  }

  TEST_CASE("TrackColumnWidthSolver - clamps flexible resizing to the representable width",
            "[uimodel][unit][library][presentation]")
  {
    auto const specs = std::vector{
      fixed(rt::TrackField::Duration, 100),
      flexible(rt::TrackField::Title, 1.0, 100, 100),
      flexible(rt::TrackField::Artist, 1.0, 100, 100),
    };

    auto const resized = resizeTrackColumnSpecs(specs, rt::TrackField::Title, 350, 400);
    auto const widths = solveTrackColumnWidths(resized, 400);

    REQUIRE(widths.size() == 3);
    CHECK(widths[0] == 100);
    CHECK(widths[1] == 200);
    CHECK(widths[2] == 100);
    CHECK(totalWidth(widths) == 400);
  }

  TEST_CASE("TrackColumnWidthSolver - builds pixel specs and canonical layout states",
            "[uimodel][unit][library][presentation]")
  {
    auto const fields = std::vector{rt::TrackField::Title, rt::TrackField::Duration};
    auto const stored = std::vector{
      TrackColumnState{.field = rt::TrackField::Title, .width = 321, .weight = 2.34567},
      TrackColumnState{.field = rt::TrackField::Duration, .width = 95, .weight = 4.0},
    };

    auto const specs = pixelTrackColumnSpecs(fields, stored);

    REQUIRE(specs.size() == 2);
    CHECK(specs[0].field == rt::TrackField::Title);
    CHECK(specs[0].fixedWidth == -1);
    CHECK(specs[0].weight == 2.34567);
    CHECK(specs[1].field == rt::TrackField::Duration);
    CHECK(specs[1].fixedWidth == 95);
    CHECK(specs[1].weight == -1.0);

    auto const titleState = canonicalTrackColumnState(specs[0]);
    auto const durationState = canonicalTrackColumnState(specs[1]);

    CHECK(titleState.field == rt::TrackField::Title);
    CHECK(titleState.width == -1);
    CHECK(titleState.weight == 2.346);
    CHECK(durationState.field == rt::TrackField::Duration);
    CHECK(durationState.width == 95);
    CHECK(durationState.weight == -1.0);
  }

  TEST_CASE("TrackColumnWidthSolver - falls back to preferred widths before a viewport exists",
            "[uimodel][unit][library][presentation]")
  {
    auto const specs = std::vector{
      fixed(rt::TrackField::Duration, 80),
      flexible(rt::TrackField::Title, 1.0, 120, 72),
    };

    auto const widths = solveTrackColumnWidths(specs, 0);

    REQUIRE(widths.size() == 2);
    CHECK(widths[0] == 80);
    CHECK(widths[1] == 120);
  }
} // namespace ao::uimodel::test
