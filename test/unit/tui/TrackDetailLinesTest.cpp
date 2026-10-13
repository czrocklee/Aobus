// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/TrackDetailLines.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/library/Credits.h>
#include <ao/library/RecordingDate.h>
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
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
                          .work = "Piano Concerto",
                          .movement = "Adagio",
                          .soloist = "Soloist",
                          .tags = "favourite",
                          .duration = std::chrono::seconds{299},
                          .conductorCount = 1,
                          .ensembleCount = 1,
                          .soloistCount = 1,
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

  TEST_CASE("TrackDetailLines - identity and facts have separate labeled rows", "[tui][unit][detail]")
  {
    using Kind = TrackDetailLine::Kind;
    auto const lines = trackDetailLines(ao::test::englishMessageCatalog(), fullyPopulatedRow());
    constexpr auto kLabels = std::array<std::string_view, 15>{
      "Title",
      "Artist",
      "Album",
      "Year",
      "Track",
      "Duration",
      "Album Artist",
      "Composer",
      "Conductor",
      "Ensemble",
      "Soloist",
      "Work",
      "Movement",
      "Genre",
      "Tags",
    };
    REQUIRE(lines.size() == kLabels.size());

    for (std::size_t index = 0; index < kLabels.size(); ++index)
    {
      CHECK(lines[index].label == kLabels[index]);
      CHECK(lines[index].kind == (index == 0 ? Kind::Title : index == 14 ? Kind::Tags : Kind::Metadata));
    }

    CHECK(lines[0].kind == Kind::Title);
    CHECK(lines[0].label == "Title");
    CHECK(lines[0].value == "Seven");
    CHECK(lines[1].label == "Artist");
    CHECK(lines[1].value == "Aimer");
    CHECK(lines[2].label == "Album");
    CHECK(lines[2].value == "Midnight Sun");
    CHECK(lines[3].label == "Year");
    CHECK(lines[3].value == "2014");
    CHECK(valueFor(lines, "Track") == "7 / 12");
    CHECK(valueFor(lines, "Duration") == "4:59");
    CHECK(valueFor(lines, "Work") == "Piano Concerto");
    CHECK(valueFor(lines, "Movement") == "Adagio");
    CHECK(valueFor(lines, "Conductor") == "Conductor");
    CHECK(valueFor(lines, "Ensemble") == "Ensemble");
    CHECK(valueFor(lines, "Soloist") == "Soloist");
    CHECK(lines.back().kind == Kind::Tags);
    CHECK(lines.back().value == "favourite");
    auto const german = ao::test::messageCatalog("de-DE");
    CHECK(hasLabel(trackDetailLines(german, fullyPopulatedRow()), "Dirigent"));
  }

  TEST_CASE("TrackDetailLines - labeled fields keep a stable sizing schema", "[tui][unit][detail]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();
    auto row = fullyPopulatedRow();
    row.channels = 2;
    row.bitrate = 1000;
    row.fileSize = 1024;
    auto lines = trackDetailLines(catalog, row);
    auto const technical = trackDetailTechnicalLines(catalog, row);
    constexpr auto kTechnicalLabels = std::array<std::string_view, 6>{
      "Codec",
      "Sample Rate",
      "Bit Depth",
      "Channels",
      "Bitrate",
      "File Size",
    };
    REQUIRE(technical.size() == kTechnicalLabels.size());

    for (std::size_t index = 0; index < kTechnicalLabels.size(); ++index)
    {
      CHECK(technical[index].label == kTechnicalLabels[index]);
      CHECK(technical[index].kind == TrackDetailLine::Kind::Technical);
    }

    lines.insert(lines.end(), technical.begin(), technical.end());

    for (auto const field : trackDetailFields())
    {
      CHECK(hasLabel(lines, uimodel::trackFieldLabel(catalog, field)));
    }
  }

  TEST_CASE("TrackDetailLines - sparse identity has no placeholders or duplicate album artist", "[tui][unit][detail]")
  {
    auto row = rt::TrackRow{.id = TrackId{3}, .title = "Untagged"};
    auto const& catalog = ao::test::englishMessageCatalog();
    auto lines = trackDetailLines(catalog, row);
    REQUIRE(lines.size() == 1);
    CHECK(lines.front().value == "Untagged");
    row.artist = "Artist";
    row.albumArtist = "Artist";
    lines = trackDetailLines(catalog, row);
    CHECK_FALSE(hasLabel(lines, "Album Artist"));
    row.albumArtist = "Various Artists";
    CHECK(hasLabel(trackDetailLines(catalog, row), "Album Artist"));
    row.duration = std::chrono::seconds{61};
    lines = trackDetailLines(catalog, row);
    CHECK(valueFor(lines, "Duration") == "1:01");
    CHECK_FALSE(hasLabel(lines, "Year"));
    CHECK_FALSE(hasLabel(lines, "Track"));
  }

  TEST_CASE("TrackDetailLines - missing metadata title uses a separately labeled filename", "[tui][unit][detail]")
  {
    auto const row = rt::TrackRow{.id = TrackId{3}, .optUriPath = "/music/untitled.flac"};
    auto const& catalog = ao::test::englishMessageCatalog();
    auto const lines = trackDetailLines(catalog, row);
    REQUIRE(lines.size() == 1);
    CHECK(lines.front().kind == TrackDetailLine::Kind::Title);
    CHECK(valueFor(lines, "File Name") == "untitled.flac");
    CHECK_FALSE(hasLabel(lines, "Title"));
  }

  TEST_CASE("TrackDetailLines - metadata title takes precedence over the filename", "[tui][unit][detail]")
  {
    auto const row = rt::TrackRow{.optUriPath = "/music/untitled.flac", .title = "Real title"};
    auto const lines = trackDetailLines(ao::test::englishMessageCatalog(), row);
    REQUIRE(lines.size() == 1);
    CHECK(lines.front().kind == TrackDetailLine::Kind::Title);
    CHECK(valueFor(lines, "Title") == "Real title");
    CHECK_FALSE(hasLabel(lines, "File Name"));
  }

  TEST_CASE("TrackDetailLines - missing title without a usable filename has no identity row", "[tui][unit][detail]")
  {
    auto row = rt::TrackRow{.id = TrackId{3}, .optUriPath = "/music/untitled.flac"};
    auto const& catalog = ao::test::englishMessageCatalog();

    SECTION("absent path")
    {
      row.optUriPath.reset();
    }

    SECTION("empty path")
    {
      row.optUriPath = std::filesystem::path{};
    }

    SECTION("path ending in a separator")
    {
      row.optUriPath = "/music/";
    }

    CHECK(trackDetailLines(catalog, row).empty());
  }

  TEST_CASE("TrackDetailLines - track numbering retains disc and total context", "[tui][unit][track-detail]")
  {
    auto row = fullyPopulatedRow();
    row.discNumber = 2;
    row.discTotal = 3;
    auto const& catalog = ao::test::englishMessageCatalog();
    CHECK(valueFor(trackDetailLines(catalog, row), "Track") == "2-7 / 12");
    row.trackTotal = 0;
    CHECK(valueFor(trackDetailLines(catalog, row), "Track") == "2-7");
  }

  TEST_CASE("TrackDetailLines - recording date keeps its precision and does not infer the release year",
            "[tui][unit][detail]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();
    auto row = rt::TrackRow{.id = TrackId{4}, .title = "Goldberg", .year = 2014};

    SECTION("an absent date does not appear beside a release year")
    {
      auto const lines = trackDetailLines(catalog, row);
      CHECK(valueFor(lines, "Year") == "2014");
      CHECK_FALSE(hasLabel(lines, "Recording Date"));
    }

    SECTION("year precision stays beside the independent release year")
    {
      row.recordingDate = {.year = 1955};
      auto const lines = trackDetailLines(catalog, row);
      auto const year = std::ranges::find(lines, std::string_view{"Year"}, &TrackDetailLine::label);
      auto const date = std::ranges::find(lines, std::string_view{"Recording Date"}, &TrackDetailLine::label);
      REQUIRE(year != lines.end());
      REQUIRE(date != lines.end());
      CHECK(year < date);
      CHECK(year->value == "2014");
      CHECK(date->value == "1955");
    }

    SECTION("month precision")
    {
      row.recordingDate = {.year = 1981, .month = 5};
      auto const lines = trackDetailLines(catalog, row);
      CHECK(valueFor(lines, "Year") == "2014");
      CHECK(valueFor(lines, "Recording Date") == "1981-05");
    }

    SECTION("day precision")
    {
      row.recordingDate = {.year = 1981, .month = 5, .day = 12};
      auto const lines = trackDetailLines(catalog, row);
      CHECK(valueFor(lines, "Year") == "2014");
      CHECK(valueFor(lines, "Recording Date") == "1981-05-12");
    }

    SECTION("a present date does not invent a release year")
    {
      row.year = 0;
      row.recordingDate = {.year = 1981, .month = 5, .day = 12};
      auto const lines = trackDetailLines(catalog, row);
      CHECK_FALSE(hasLabel(lines, "Year"));
      CHECK(valueFor(lines, "Recording Date") == "1981-05-12");
    }
  }

  TEST_CASE("TrackDetailLines - credit lines keep kinds ordered duplicates roles and independently mixed sections",
            "[tui][unit][detail]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();
    auto sections = uimodel::TrackCreditSections{};
    sections[3].optValue = std::vector<library::Credit>{{"Gould", library::CreditKind::Performer, "Piano"},
                                                        {"Gould", library::CreditKind::Performer, ""},
                                                        {"Gould", library::CreditKind::Performer, "Piano"}};
    auto const lines = trackCreditDetailLines(catalog, sections);
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].label == "Credits");
    CHECK(lines[0].value.empty());
    CHECK(lines[1].label == "Performer");
    CHECK(lines[1].value == "Gould (Piano)");
    CHECK(lines[2].label == "Performer");
    CHECK(lines[2].value == "Gould");
    CHECK(lines[3].label == "Performer");
    CHECK(lines[3].value == "Gould (Piano)");
    sections[0].mixed = true;
    auto const mixed = trackCreditDetailLines(catalog, sections);
    REQUIRE(mixed.size() == 5);
    CHECK(mixed[1].label == "Conductor");
    CHECK(mixed[1].value == "<Multiple Values>");
    CHECK(mixed[2].value == "Gould (Piano)");
    CHECK(trackCreditDetailLines(catalog, {}).empty());
  }

  TEST_CASE("TrackDetailLines - category summaries count duplicate entries", "[tui][unit][detail]")
  {
    auto row = rt::TrackRow{.conductor = "A", .conductorCount = 3};
    auto const lines = trackDetailLines(ao::test::englishMessageCatalog(), row);
    CHECK(valueFor(lines, "Conductor") == "A +2");
  }
} // namespace ao::tui::test
