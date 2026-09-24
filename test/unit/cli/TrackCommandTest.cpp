// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace ao::cli::test
{
  namespace
  {
    std::size_t countJsonLinesWithField(std::string_view lines, std::string_view field, std::string_view value)
    {
      std::size_t count = 0;
      std::size_t lineStart = 0;

      while (lineStart < lines.size())
      {
        auto const end = lines.find('\n', lineStart);
        auto const line =
          lines.substr(lineStart, end == std::string_view::npos ? std::string_view::npos : end - lineStart);

        if (!line.empty())
        {
          auto tree = parseYaml(line);
          REQUIRE(tree.rootref().is_map());

          if (yaml::scalarView(yaml::findChild(tree.rootref(), field)) == value)
          {
            ++count;
          }
        }

        if (end == std::string_view::npos)
        {
          break;
        }

        lineStart = end + 1;
      }

      return count;
    }
  } // namespace

  TEST_CASE("CLI - track show format expression streams formatted rows", "[cli][unit][track][format]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");
    fixture.copyAudio("hires.flac", "hires.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"track",
                          "show",
                          "--filter",
                          "$title ~ \"Test\"",
                          "--format",
                          R"format($artist + " - " + $title + " (" + @codec + ")")format"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(result.out == "Test Artist - Test Title (FLAC)\n");

    checkDomainFailure(fixture.run({"track", "show", "--format", "#fav"}), "format error:");
    checkDomainFailure(fixture.run({"-O", "json", "track", "show", "--format", "$title"}),
                       "track show --format supports only plain output");
  }

  TEST_CASE("CLI - track show resolves explicit id batches directly", "[cli][unit][track][show]")
  {
    auto fixture = CliFixture{};
    auto const first = fixture.addTrack(library::test::TrackSpec{.title = "First", .uri = "first.flac"});
    fixture.addTrack(library::test::TrackSpec{.title = "Second", .uri = "second.flac"});
    auto const third = fixture.addTrack(library::test::TrackSpec{.title = "Third", .uri = "third.flac"});

    auto result = fixture.run(
      {"track", "show", std::to_string(third.raw()), std::to_string(first.raw()), std::to_string(third.raw())});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(countOccurrences(result.out, "\n") == 2);

    auto const thirdOffset = result.out.find("Third");
    auto const firstOffset = result.out.find("First");
    REQUIRE(thirdOffset != std::string::npos);
    REQUIRE(firstOffset != std::string::npos);
    CHECK(thirdOffset < firstOffset);
    CHECK_FALSE(contains(result.out, "Second"));
  }

  TEST_CASE("CLI - track create imports one file", "[cli][integration][track][create]")
  {
    auto fixture = CliFixture{};

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    fixture.copyAudio("basic_metadata.flac", "created.flac");
    result = fixture.run({"track", "create", (fixture.root() / "created.flac").string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "added track:"));

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Test Title"));
  }

  TEST_CASE("CLI - track delete removes the track from subsequent output", "[cli][integration][track]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"track", "delete", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "deleted track: 1"));

    result = fixture.run({"-O", "yaml", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK_FALSE(contains(result.out, "Test Title"));

    result = fixture.run({"scan"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "new 1"));

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Test Title"));
  }

  TEST_CASE("CLI - track update edits metadata and reports no-op patches", "[cli][integration][track][update]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"track", "update", "1", "--title", "Renamed"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "updated 1 of 1 matched track(s)"));

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Renamed"));
    CHECK_FALSE(contains(result.out, "Test Title"));

    result = fixture.run({"track", "update", "1", "--title", "Renamed"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "updated 0 of 1 matched track(s)"));
  }

  TEST_CASE("CLI - track update edits explicit id batches", "[cli][integration][track][update]")
  {
    auto fixture = CliFixture{};
    auto const firstId = fixture.addTrack(library::test::makeEmptyTrackSpec("first.flac"));
    auto const secondId = fixture.addTrack(library::test::makeEmptyTrackSpec("second.flac"));
    auto const thirdId = fixture.addTrack(library::test::makeEmptyTrackSpec("third.flac"));

    auto result = fixture.run(
      {"track", "update", std::to_string(firstId.raw()), std::to_string(secondId.raw()), "--genre", "Classical"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "updated 2 of 2 matched track(s)"));

    result = fixture.run({"-O", "json", "track", "show", "--filter", "$genre = \"Classical\""});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(countJsonLinesWithField(result.out, "id", std::to_string(firstId.raw())) == 1);
    CHECK(countJsonLinesWithField(result.out, "id", std::to_string(secondId.raw())) == 1);
    CHECK(countJsonLinesWithField(result.out, "id", std::to_string(thirdId.raw())) == 0);
  }

  TEST_CASE("CLI - track update edits filtered batches with structured output", "[cli][integration][track][update]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");
    fixture.copyAudio("hires.flac", "hires.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "json", "track", "update", "--filter", "$title ~ \"Title\"", "--artist", "Unified"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    requireJsonLineParses(result.out);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["matched"]) == "2");
    CHECK(yaml::scalarView(tree.rootref()["updated"]) == "2");
    REQUIRE(tree.rootref()["trackIds"].is_seq());
    CHECK(tree.rootref()["trackIds"].num_children() == 2);
    CHECK(yaml::scalarView(tree.rootref()["changes"][0]["fields"][0]["field"]) == "artist");

    result = fixture.run({"-O", "json", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(countJsonLinesWithField(result.out, "artist", "Unified") == 2);

    result = fixture.run({"track", "update", "--filter", "$artist = \"Unified\"", "--artist", "Unified"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "updated 0 of 2 matched track(s)"));
  }

  TEST_CASE("CLI - track show exposes writable metadata fields", "[cli][integration][track][output]")
  {
    auto fixture = CliFixture{};
    auto const trackId = fixture.addTrack(library::test::makeEmptyTrackSpec("track.flac"));

    auto result = fixture.run({"track",
                               "update",
                               std::to_string(trackId.raw()),
                               "--album-artist",
                               "Album Artist",
                               "--genre",
                               "Jazz",
                               "--composer",
                               "Composer",
                               "--work",
                               "Work",
                               "--movement",
                               "Finale",
                               "--year",
                               "1984",
                               "--track-number",
                               "7",
                               "--track-total",
                               "11",
                               "--disc-number",
                               "2",
                               "--disc-total",
                               "3",
                               "--movement-number",
                               "4",
                               "--movement-total",
                               "5"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());

    result = fixture.run({"-O", "json", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    auto tree = parseYaml(result.out);
    auto root = tree.rootref();
    CHECK(yaml::scalarView(root["albumArtist"]) == "Album Artist");
    CHECK(yaml::scalarView(root["genre"]) == "Jazz");
    CHECK(yaml::scalarView(root["composer"]) == "Composer");
    CHECK(yaml::scalarView(root["work"]) == "Work");
    CHECK(yaml::scalarView(root["movement"]) == "Finale");
    CHECK(yaml::scalarView(root["year"]) == "1984");
    CHECK(yaml::scalarView(root["trackNumber"]) == "7");
    CHECK(yaml::scalarView(root["trackTotal"]) == "11");
    CHECK(yaml::scalarView(root["discNumber"]) == "2");
    CHECK(yaml::scalarView(root["discTotal"]) == "3");
    CHECK(yaml::scalarView(root["movementNumber"]) == "4");
    CHECK(yaml::scalarView(root["movementTotal"]) == "5");

    result = fixture.run({"-O", "json", "track", "show", "--filter", "$movement = \"Finale\""});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["movement"]) == "Finale");

    result = fixture.run({"-O", "json", "track", "show", std::to_string(trackId.raw())});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["id"]) == std::to_string(trackId.raw()));

    result = fixture.run({"track", "show", std::to_string(trackId.raw()), "--format", "$genre + \": \" + $movement"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "Jazz: Finale"));

    result = fixture.run({"track", "show", std::to_string(trackId.raw()), "--filter", "$genre = \"Jazz\""});
    checkDomainFailure(result, "track show accepts either explicit ids or --filter");
  }

  TEST_CASE("CLI - missing field filters agree with structured omissions", "[cli][unit][track][output]")
  {
    auto fixture = CliFixture{};
    fixture.addTrack(library::test::makeEmptyTrackSpec("missing.flac"));
    fixture.addTrack(library::test::TrackSpec{.title = "Known", .genre = "Known", .uri = "known.flac"});

    auto result = fixture.run({"-O", "json", "track", "show", "--filter", "not $genre?"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(countOccurrences(result.out, "\n") == 1);

    auto tree = parseYaml(result.out);
    auto root = tree.rootref();
    CHECK(yaml::scalarView(root["uri"]) == "missing.flac");
    CHECK_FALSE(root["genre"].readable());
    CHECK_FALSE(root["year"].readable());
    CHECK_FALSE(root["trackNumber"].readable());
  }

  TEST_CASE("CLI - genre repair loop converges through filters and structured output", "[cli][integration][track]")
  {
    auto fixture = CliFixture{};
    fixture.addTrack(library::test::TrackSpec{.title = "Missing One", .genre = "", .uri = "missing-one.flac"});
    fixture.addTrack(library::test::TrackSpec{.title = "Missing Two", .genre = "", .uri = "missing-two.flac"});
    fixture.addTrack(library::test::TrackSpec{.title = "Known", .genre = "Known", .uri = "known.flac"});

    auto result = fixture.run({"-O", "json", "track", "show", "--filter", "not $genre?"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(countOccurrences(result.out, "\n") == 2);
    CHECK(contains(result.out, "Missing One"));
    CHECK(contains(result.out, "Missing Two"));

    result =
      fixture.run({"-O", "json", "track", "update", "--dry-run", "--filter", "not $genre?", "--genre", "Inferred"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["matched"]) == "2");
    CHECK(yaml::scalarView(tree.rootref()["updated"]) == "2");
    CHECK(yaml::scalarView(tree.rootref()["changes"][0]["fields"][0]["newValue"]) == "Inferred");

    result = fixture.run({"-O", "json", "track", "show", "--filter", "not $genre?"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(countOccurrences(result.out, "\n") == 2);

    result = fixture.run({"-O", "json", "track", "update", "--filter", "not $genre?", "--genre", "Inferred"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");
    CHECK(yaml::scalarView(tree.rootref()["matched"]) == "2");
    CHECK(yaml::scalarView(tree.rootref()["updated"]) == "2");

    result = fixture.run({"-O", "json", "track", "show", "--filter", "not $genre?"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(result.out.empty());

    result = fixture.run({"-O", "json", "track", "show", "--filter", "$genre = \"Inferred\""});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(countJsonLinesWithField(result.out, "genre", "Inferred") == 2);
  }

  TEST_CASE("CLI - track update sets and unsets custom metadata", "[cli][integration][track][update]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"track", "update", "1", "--set", "mood=bright", "--set", "energy=high"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "updated 1 of 1 matched track(s)"));

    result = fixture.run({"-O", "json", "track", "show"});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["custom"]["mood"]) == "bright");
    CHECK(yaml::scalarView(tree.rootref()["custom"]["energy"]) == "high");

    result = fixture.run({"track", "update", "1", "--unset", "mood"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "updated 1 of 1 matched track(s)"));

    result = fixture.run({"-O", "json", "track", "show"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK_FALSE(tree.rootref()["custom"]["mood"].readable());
    CHECK(yaml::scalarView(tree.rootref()["custom"]["energy"]) == "high");
  }

  TEST_CASE("CLI - track mutations support dry-run reports", "[cli][integration][track][dry-run]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"-O", "json", "track", "create", "--dry-run", "track.flac"});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["action"]) == "create");
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK_FALSE(tree.rootref()["trackId"].readable());
    CHECK(yaml::scalarView(tree.rootref()["uri"]) == "track.flac");

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(result.out.empty());

    result = fixture.run({"-O", "json", "track", "create", "track.flac"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");
    CHECK(yaml::scalarView(tree.rootref()["trackId"]) == "1");

    result = fixture.run({"-O", "json", "track", "update", "--dry-run", "1", "--title", "Renamed"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["matched"]) == "1");
    CHECK(yaml::scalarView(tree.rootref()["changes"][0]["fields"][0]["oldValue"]) == "Test Title");
    CHECK(yaml::scalarView(tree.rootref()["changes"][0]["fields"][0]["newValue"]) == "Renamed");

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Test Title"));
    CHECK_FALSE(contains(result.out, "Renamed"));

    result = fixture.run({"-O", "json", "track", "update", "1", "--title", "Renamed"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Renamed"));

    result = fixture.run({"-O", "json", "track", "delete", "--dry-run", "1"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "Renamed");

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Renamed"));

    result = fixture.run({"-O", "json", "track", "delete", "1"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(result.out.empty());
  }

  TEST_CASE("CLI - missing track dump id reports an error for every dump format", "[cli][unit][track][contract]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    checkDomainFailure(fixture.run({"track", "dump", "--id", "999"}), "track not found: 999");
    checkDomainFailure(fixture.run({"track", "dump", "--id", "999", "--raw"}), "track not found: 999");
    checkDomainFailure(fixture.run({"-O", "yaml", "track", "dump", "--id", "999"}), "track dump supports only plain");
  }

  TEST_CASE("CLI - structured track output quotes strings and parses", "[cli][unit][track][output]")
  {
    auto fixture = CliFixture{};
    auto title = std::string{"Quote \"Title\"\nSecond Line"};
    fixture.addTrack(library::test::TrackSpec{.title = title,
                                              .artist = "Artist \\ Name",
                                              .album = "Album",
                                              .uri = "special.flac",
                                              .tags = {"fav"},
                                              .customMetadata = {{"mood", "bright\nsharp"}}});

    auto result = fixture.run({"-O", "yaml", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    auto tree = parseYaml(result.out);
    auto tracks = tree.rootref()["tracks"];
    REQUIRE(tracks.is_seq());
    REQUIRE(tracks.num_children() == 1);
    CHECK(yaml::scalarView(tracks[0]["title"]) == title);
    CHECK(yaml::scalarView(tracks[0]["custom"]["mood"]) == "bright\nsharp");

    result = fixture.run({"-O", "json", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    requireJsonLineParses(result.out);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["title"]) == title);
    CHECK(yaml::scalarView(tree.rootref()["custom"]["mood"]) == "bright\nsharp");
  }

  TEST_CASE("CLI - track domain failures use stderr and exit non-zero", "[cli][unit][track][contract]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    checkDomainFailure(fixture.run({"track", "show", "--filter", "("}), "filter error:");
    checkDomainFailure(fixture.run({"track", "show", "--filter", "$missingField = 1"}), "filter error:");
    checkDomainFailure(
      fixture.run({"track", "create", (fixture.root() / "missing.flac").string()}), "error adding track from:");
    checkDomainFailure(fixture.run({"track", "delete", "999"}), "track not found: 999");
    checkDomainFailure(fixture.run({"track", "update", "999", "--title", "Missing"}), "track not found: 999");
    checkDomainFailure(fixture.run({"track", "update", "1"}), "track update requires at least one field option");
    checkDomainFailure(fixture.run({"track", "update", "--title", "Missing"}), "track update requires track ids");
  }
} // namespace ao::cli::test
