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

    REQUIRE(index.tryReset(5, sections));
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
    CHECK(index.itemAt(4) == TrackDisplayItem{
                               .kind = TrackDisplayItemKind::TrackRow,
                               .sourceIndex = 2,
                               .groupIndex = 1,
                             });
    CHECK(index.itemAt(5) == TrackDisplayItem{
                               .kind = TrackDisplayItemKind::TrackRow,
                               .sourceIndex = 3,
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
    CHECK(index.displayIndexOfSourceRow(3) == 5);
    CHECK(index.displayIndexOfSourceRow(4) == 6);
    CHECK_FALSE(index.displayIndexOfSourceRow(5));
  }

  TEST_CASE("TrackDisplayIndex - rejects malformed sections without replacing the valid mapping",
            "[uimodel][unit][track-display]")
  {
    auto index = TrackDisplayIndex{};
    REQUIRE(index.tryReset(2, {}));

    SECTION("leading gap")
    {
      auto const gap = std::to_array<TrackDisplaySection>({{.start = 1, .count = 1}});
      CHECK_FALSE(index.tryReset(2, gap));
    }

    SECTION("empty group")
    {
      auto const sections = std::to_array<TrackDisplaySection>({{.start = 0, .count = 0}, {.start = 0, .count = 2}});
      CHECK_FALSE(index.tryReset(2, sections));
    }

    SECTION("overrun")
    {
      auto const sections = std::to_array<TrackDisplaySection>({{.start = 0, .count = 3}});
      CHECK_FALSE(index.tryReset(2, sections));
    }

    SECTION("incomplete coverage")
    {
      auto const sections = std::to_array<TrackDisplaySection>({{.start = 0, .count = 1}});
      CHECK_FALSE(index.tryReset(3, sections));
    }

    SECTION("overlapping groups")
    {
      auto const sections = std::to_array<TrackDisplaySection>({{.start = 0, .count = 1}, {.start = 0, .count = 1}});
      CHECK_FALSE(index.tryReset(2, sections));
    }

    CHECK(index.rowCount() == 2);
    CHECK(index.groupCount() == 0);
    CHECK(index.displayCount() == 2);
    CHECK(index.itemAt(0) == TrackDisplayItem{
                               .kind = TrackDisplayItemKind::TrackRow,
                               .sourceIndex = 0,
                               .groupIndex = 0,
                             });
    CHECK_FALSE(index.itemAt(2));
    CHECK(index.itemAt(1) == TrackDisplayItem{
                               .kind = TrackDisplayItemKind::TrackRow,
                               .sourceIndex = 1,
                               .groupIndex = 0,
                             });
    CHECK(index.displayIndexOfSourceRow(0) == 0);
    CHECK(index.displayIndexOfSourceRow(1) == 1);
    CHECK_FALSE(index.displayIndexOfSourceRow(2));
  }

  TEST_CASE("TrackDisplayIndex - clear discards both row and group coordinates", "[uimodel][unit][track-display]")
  {
    auto index = TrackDisplayIndex{};
    auto const sections = std::to_array<TrackDisplaySection>({{.start = 0, .count = 2}});
    REQUIRE(index.tryReset(2, sections));
    REQUIRE(index.rowCount() == 2);
    REQUIRE(index.groupCount() == 1);
    REQUIRE(index.displayCount() == 3);

    index.clear();

    CHECK(index.rowCount() == 0);
    CHECK(index.groupCount() == 0);
    CHECK(index.displayCount() == 0);
    CHECK_FALSE(index.itemAt(0));
    CHECK_FALSE(index.displayIndexOfSourceRow(0));

    REQUIRE(index.tryReset(1, {}));
    CHECK(index.rowCount() == 1);
    CHECK(index.groupCount() == 0);
    CHECK(index.displayCount() == 1);
    CHECK(index.itemAt(0) == TrackDisplayItem{
                               .kind = TrackDisplayItemKind::TrackRow,
                               .sourceIndex = 0,
                               .groupIndex = 0,
                             });
    CHECK(index.displayIndexOfSourceRow(0) == 0);
    CHECK_FALSE(index.itemAt(1));
    CHECK_FALSE(index.displayIndexOfSourceRow(1));
  }
} // namespace ao::uimodel::test
