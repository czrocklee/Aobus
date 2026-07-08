// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/TrackDetailLines.h"

#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/rt/TrackRow.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::tui::test
{
  TEST_CASE("TrackDetailLines - expose user-facing metadata", "[tui][unit][track-detail]")
  {
    auto row = rt::TrackRow{.id = TrackId{9},
                            .title = "Seven",
                            .artist = "Aimer",
                            .album = "Midnight Sun",
                            .albumArtist = "Various",
                            .genre = "Rock",
                            .composer = "Composer",
                            .conductor = "Conductor",
                            .ensemble = "Ensemble",
                            .soloist = "Soloist",
                            .duration = std::chrono::seconds{299},
                            .year = 2014,
                            .trackNumber = 7,
                            .trackTotal = 12,
                            .sampleRate = 44100,
                            .bitDepth = 16,
                            .codec = AudioCodec::Flac};

    auto lines = trackDetailLines(row);

    REQUIRE(lines.size() >= 15);
    CHECK(lines[0].label == "Title");
    CHECK(lines[0].value == "Seven");
    CHECK(lines[1].value == "Aimer");
    CHECK(lines[5].label == "Conductor");
    CHECK(lines[5].value == "Conductor");
    CHECK(lines[6].label == "Ensemble");
    CHECK(lines[6].value == "Ensemble");
    CHECK(lines[7].label == "Soloist");
    CHECK(lines[7].value == "Soloist");
    CHECK(lines[9].value == "2014");
    CHECK(lines[10].value == "7");
    CHECK(lines[11].value == "4:59");
    CHECK(lines[13].value == "44100 Hz");
    CHECK(lines[14].value == "16-bit");
  }
} // namespace ao::tui::test
