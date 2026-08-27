// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/TrackDetailLines.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/presentation/PresentationText.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string_view>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    rt::TrackRow fullyPopulatedRow()
    {
      return rt::TrackRow{.id = TrackId{9},
                          .title = "Seven",
                          .artist = "Aimer",
                          .album = "Midnight Sun",
                          .albumArtist = "Various",
                          .genre = "Rock",
                          .composer = "Composer",
                          .conductor = "Conductor",
                          .ensemble = "Ensemble",
                          .soloist = "Soloist",
                          .tags = "favourite",
                          .duration = std::chrono::seconds{299},
                          .year = 2014,
                          .trackNumber = 7,
                          .trackTotal = 12,
                          .sampleRate = 44100,
                          .bitDepth = 16,
                          .codec = AudioCodec::Flac};
    }

    bool hasLabel(std::vector<TrackDetailLine> const& lines, std::string_view const label)
    {
      return std::ranges::any_of(lines, [label](TrackDetailLine const& line) { return line.label == label; });
    }

    std::string_view valueFor(std::vector<TrackDetailLine> const& lines, std::string_view const label)
    {
      auto const it = std::ranges::find(lines, label, &TrackDetailLine::label);
      return it == lines.end() ? std::string_view{} : std::string_view{it->value};
    }
  } // namespace

  TEST_CASE("TrackDetailLines - expose user-facing metadata", "[tui][unit][track-detail]")
  {
    auto const lines = trackDetailLines(ao::test::englishMessageCatalog(), fullyPopulatedRow());

    CHECK(lines.size() == trackDetailFields().size());
    CHECK(lines[0].label == "Title");
    CHECK(lines[0].value == "Seven");
    CHECK(valueFor(lines, "Artist") == "Aimer");
    CHECK(valueFor(lines, "Conductor") == "Conductor");
    CHECK(valueFor(lines, "Ensemble") == "Ensemble");
    CHECK(valueFor(lines, "Soloist") == "Soloist");
    CHECK(valueFor(lines, "Year") == "2014");
    CHECK(valueFor(lines, "Duration") == "4:59");
    CHECK(valueFor(lines, "Sample Rate") == "44100 Hz");
    CHECK(valueFor(lines, "Bit Depth") == "16-bit");
    CHECK(valueFor(lines, "Tags") == "favourite");

    auto const german = ao::test::messageCatalog("de-DE");
    auto const germanLines = trackDetailLines(german, fullyPopulatedRow());
    CHECK(germanLines[0].label == "Titel");
    CHECK(germanLines[0].value == "Seven");
    CHECK(hasLabel(germanLines, "Dirigent"));
  }

  TEST_CASE("TrackDetailLines - keep every field a fully tagged track carries", "[tui][unit][track-detail]")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto const lines = trackDetailLines(textCatalog, fullyPopulatedRow());
    auto lineIt = lines.begin();

    // Display order is the pane's field order, which is also what its width is
    // measured against.
    for (auto const field : trackDetailFields())
    {
      REQUIRE(lineIt != lines.end());
      CHECK(lineIt->label == uimodel::trackFieldLabel(textCatalog, field));
      ++lineIt;
    }

    CHECK(lineIt == lines.end());
  }

  TEST_CASE("TrackDetailLines - a sparse track omits optional rows instead of filling them",
            "[tui][unit][track-detail]")
  {
    auto const row = rt::TrackRow{.id = TrackId{3}, .title = "Untagged"};
    auto const lines = trackDetailLines(ao::test::englishMessageCatalog(), row);

    CHECK(hasLabel(lines, "Title"));
    CHECK(hasLabel(lines, "Artist"));
    CHECK(hasLabel(lines, "Album"));
    CHECK(hasLabel(lines, "Track #"));
    CHECK(hasLabel(lines, "Duration"));
    CHECK(valueFor(lines, "Artist") == "-");
    CHECK(valueFor(lines, "Duration") == "-");

    CHECK_FALSE(hasLabel(lines, "Album Artist"));
    CHECK_FALSE(hasLabel(lines, "Composer"));
    CHECK_FALSE(hasLabel(lines, "Conductor"));
    CHECK_FALSE(hasLabel(lines, "Ensemble"));
    CHECK_FALSE(hasLabel(lines, "Soloist"));
    CHECK_FALSE(hasLabel(lines, "Genre"));
    CHECK_FALSE(hasLabel(lines, "Year"));
    CHECK_FALSE(hasLabel(lines, "Codec"));
    CHECK_FALSE(hasLabel(lines, "Sample Rate"));
    CHECK_FALSE(hasLabel(lines, "Bit Depth"));
    CHECK_FALSE(hasLabel(lines, "Tags"));
  }
} // namespace ao::tui::test
