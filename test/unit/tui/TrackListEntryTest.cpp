// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/TrackListEntry.h"

#include <ao/CoreIds.h>
#include <ao/rt/TrackRow.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace ao::tui::test
{
  TEST_CASE("TrackListEntry - chooses useful title and detail text", "[tui][unit][track-list]")
  {
    auto row = rt::TrackRow{.id = TrackId{7},
                            .optUriPath = std::filesystem::path{"music/untitled.flac"},
                            .title = "Fugue",
                            .artist = "A. Composer",
                            .album = "Keyboard Works",
                            .duration = std::chrono::seconds{125}};

    auto item = makeTrackListEntry(row);

    CHECK(item.id == TrackId{7});
    CHECK(item.row.title == "Fugue");
    CHECK(item.detail == "A. Composer - Keyboard Works  2:05");
    CHECK(item.label == "--  Fugue  A. Composer  Keyboard Works");

    row.title.clear();
    item = makeTrackListEntry(row);

    CHECK(item.label == "--  untitled.flac  A. Composer  Keyboard Works");

    row.optUriPath.reset();
    row.id = TrackId{99};
    item = makeTrackListEntry(row);

    CHECK(item.label == "--  Track 99  A. Composer  Keyboard Works");
  }

  TEST_CASE("TrackListEntry - menu labels preserve track order", "[tui][unit][track-list]")
  {
    auto tracks =
      std::vector<TrackListEntry>{{.id = TrackId{1}, .label = "First"}, {.id = TrackId{2}, .label = "Second"}};

    CHECK(menuLabels(tracks) == std::vector<std::string>{"First", "Second"});
  }
} // namespace ao::tui::test
