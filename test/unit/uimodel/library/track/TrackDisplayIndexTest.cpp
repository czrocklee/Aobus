// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/track/TrackDisplayIndex.h>

#include <catch2/catch_test_macros.hpp>

#include <array>

namespace ao::uimodel::test
{
  TEST_CASE("TrackDisplayIndex - interleaves group headings without changing source row coordinates",
            "[uimodel][unit][track-display]")
  {
    auto index = TrackDisplayIndex{};
    auto const sections = std::to_array<TrackDisplaySection>({
      {.start = 0, .count = 2},
      {.start = 2, .count = 3},
    });

    REQUIRE(index.reset(5, sections));
    CHECK(index.displayCount() == 7);
    CHECK(index.itemAt(0) == TrackDisplayItem{
                               .kind = TrackDisplayItemKind::GroupHeader,
                               .sourceIndex = 0,
                               .groupIndex = 0,
                             });
    CHECK(index.itemAt(1) == TrackDisplayItem{
                               .kind = TrackDisplayItemKind::TrackRow,
                               .sourceIndex = 0,
                               .groupIndex = 0,
                             });
    CHECK(index.itemAt(2) == TrackDisplayItem{
                               .kind = TrackDisplayItemKind::TrackRow,
                               .sourceIndex = 1,
                               .groupIndex = 0,
                             });
    CHECK(index.itemAt(3) == TrackDisplayItem{
                               .kind = TrackDisplayItemKind::GroupHeader,
                               .sourceIndex = 2,
                               .groupIndex = 1,
                             });
    CHECK(index.itemAt(6) == TrackDisplayItem{
                               .kind = TrackDisplayItemKind::TrackRow,
                               .sourceIndex = 4,
                               .groupIndex = 1,
                             });
    CHECK_FALSE(index.itemAt(7));
    CHECK(index.displayIndexOfSourceRow(0) == 1);
    CHECK(index.displayIndexOfSourceRow(1) == 2);
    CHECK(index.displayIndexOfSourceRow(2) == 4);
    CHECK(index.displayIndexOfSourceRow(4) == 6);
    CHECK_FALSE(index.displayIndexOfSourceRow(5));
  }

  TEST_CASE("TrackDisplayIndex - rejects malformed sections without replacing the valid mapping",
            "[uimodel][unit][track-display]")
  {
    auto index = TrackDisplayIndex{};
    REQUIRE(index.reset(2, {}));

    auto const gap = std::to_array<TrackDisplaySection>({
      {.start = 1, .count = 1},
    });
    CHECK_FALSE(index.reset(2, gap));
    CHECK(index.displayCount() == 2);
    CHECK(index.itemAt(1) == TrackDisplayItem{
                               .kind = TrackDisplayItemKind::TrackRow,
                               .sourceIndex = 1,
                               .groupIndex = 0,
                             });
    CHECK(index.displayIndexOfSourceRow(0) == 0);
    CHECK(index.displayIndexOfSourceRow(1) == 1);
    CHECK_FALSE(index.displayIndexOfSourceRow(2));
  }
} // namespace ao::uimodel::test
