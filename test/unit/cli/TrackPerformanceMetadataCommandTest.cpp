// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CliTestSupport.h"
#include "Run.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/Credits.h>
#include <ao/library/RecordingDate.h>
#include <ao/rt/TrackField.h>
#include <ao/yaml/RymlAdapter.h>

#include <CLI/Error.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ao::cli::test
{
  namespace
  {
    std::string idArg(TrackId id)
    {
      return std::to_string(id.raw());
    }

    library::RecordingDate date(std::uint16_t year, std::uint8_t month = 0, std::uint8_t day = 0)
    {
      return library::RecordingDate{.year = year, .month = month, .day = day};
    }
  } // namespace

  TEST_CASE("CLI - track show reports recording date at its stored precision", "[cli][unit][track][show]")
  {
    auto fixture = CliFixture{};
    fixture.addTrack(library::test::TrackSpec{.title = "Year only", .recordingDate = date(1981), .uri = "year.flac"});
    fixture.addTrack(
      library::test::TrackSpec{.title = "Year month", .recordingDate = date(1981, 5), .uri = "yearmonth.flac"});
    fixture.addTrack(
      library::test::TrackSpec{.title = "Full date", .recordingDate = date(1981, 5, 12), .uri = "full.flac"});
    fixture.addTrack(library::test::TrackSpec{.title = "No date", .uri = "nodate.flac"});

    auto result = fixture.run({"-O", "yaml", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    auto tree = parseYaml(result.out);
    auto const tracks = tree.rootref()["tracks"];
    REQUIRE(tracks.is_seq());
    REQUIRE(tracks.num_children() == 4);
    CHECK(yaml::scalarView(tracks[0]["recordingDate"]) == "1981");
    CHECK(yaml::scalarView(tracks[1]["recordingDate"]) == "1981-05");
    CHECK(yaml::scalarView(tracks[2]["recordingDate"]) == "1981-05-12");
    CHECK_FALSE(tracks[3].has_child("recordingDate"));

    result = fixture.run({"-O", "json", "track", "show", "--filter", "not $recordingDate?"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    requireJsonLineParses(result.out);
    auto jsonTree = parseYaml(result.out);
    CHECK(yaml::scalarView(jsonTree.rootref()["title"]) == "No date");
    CHECK_FALSE(jsonTree.rootref()["recordingDate"].readable());
  }

  TEST_CASE("CLI - track show reports one canonical credits sequence", "[cli][unit][track][show]")
  {
    auto fixture = CliFixture{};
    fixture.addTrack(library::test::TrackSpec{
      .title = "Goldberg",
      .credits = {{.name = "Glenn Gould", .kind = library::CreditKind::Soloist, .role = "piano"},
                  {.name = "A=B"},
                  {.name = "Glenn Gould", .kind = library::CreditKind::Soloist, .role = "piano"},
                  {.name = "Leonard Bernstein", .kind = library::CreditKind::Conductor}},
      .uri = "goldberg.flac"});
    fixture.addTrack(library::test::TrackSpec{.title = "No list", .uri = "nolist.flac"});

    auto result = fixture.run({"-O", "yaml", "track", "show", "--filter", "$credit? and $title = \"Goldberg\""});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    auto tree = parseYaml(result.out);
    auto const track = tree.rootref()["tracks"][0];
    auto const credits = track["credits"];
    REQUIRE(credits.is_seq());
    REQUIRE(credits.num_children() == 4);
    CHECK(yaml::scalarView(credits[0]["name"]) == "Leonard Bernstein");
    CHECK(yaml::scalarView(credits[0]["kind"]) == "conductor");
    CHECK_FALSE(credits[0].has_child("role"));
    CHECK(yaml::scalarView(credits[1]["name"]) == "Glenn Gould");
    CHECK(yaml::scalarView(credits[1]["kind"]) == "soloist");
    CHECK(yaml::scalarView(credits[1]["role"]) == "piano");
    CHECK(yaml::scalarView(credits[2]["name"]) == "Glenn Gould");
    CHECK(yaml::scalarView(credits[2]["kind"]) == "soloist");
    CHECK(yaml::scalarView(credits[2]["role"]) == "piano");
    CHECK(yaml::scalarView(credits[3]["name"]) == "A=B");
    CHECK(yaml::scalarView(credits[3]["kind"]) == "performer");
    CHECK_FALSE(credits[3].has_child("role"));

    for (auto const* key : {"conductor", "ensemble", "soloist", "musicians"})
    {
      CHECK_FALSE(track.has_child(key));
    }

    result = fixture.run({"-O", "json", "track", "show", "--filter", "not $credit?"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    requireJsonLineParses(result.out);
    auto jsonTree = parseYaml(result.out);
    CHECK(yaml::scalarView(jsonTree.rootref()["title"]) == "No list");
    CHECK(jsonTree.rootref()["credits"].is_seq());
    CHECK(jsonTree.rootref()["credits"].num_children() == 0);
  }

  TEST_CASE("CLI - track update parses recording date forms and clear", "[cli][integration][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackId = fixture.addTrack(library::test::makeEmptyTrackSpec("date.flac"));
    auto const trackArg = idArg(trackId);

    auto result = fixture.run({"track", "update", trackArg, "--recording-date", "1981"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "updated 1 of 1 matched track(s)"));

    result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["recordingDate"]) == "1981");

    result = fixture.run({"track", "update", trackArg, "--recording-date", "  1981-05  "});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "updated 1 of 1 matched track(s)"));
    result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["recordingDate"]) == "1981-05");

    result = fixture.run({"track", "update", trackArg, "--recording-date", "1981-05-12"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "updated 1 of 1 matched track(s)"));
    result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["recordingDate"]) == "1981-05-12");

    result = fixture.run({"track", "update", trackArg, "--recording-date", "1981-05-12"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "updated 0 of 1 matched track(s)"));
    result = fixture.run({"track", "update", trackArg, "--recording-date", "   "});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "updated 1 of 1 matched track(s)"));
    result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK_FALSE(tree.rootref()["recordingDate"].readable());
    result = fixture.run({"track", "update", trackArg, "--recording-date", ""});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "updated 0 of 1 matched track(s)"));
  }

  TEST_CASE("CLI - track update rejects invalid recording dates before mutation", "[cli][unit][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackId = fixture.addTrack(
      library::test::TrackSpec{.title = "Dated", .recordingDate = date(1981, 5, 12), .uri = "dated.flac"});
    auto const trackArg = idArg(trackId);

    for (auto const* invalid : {"1981-13",
                                "1981-00",
                                "1981-02-30",
                                "1981-5",
                                "81-05-12",
                                "0000",
                                "19810512",
                                "1981-05-12-07",
                                "9999-99",
                                "not-a-date"})
    {
      checkDomainFailure(
        fixture.run({"track", "update", trackArg, "--recording-date", invalid}), "invalid --recording-date value");
    }

    auto result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["recordingDate"]) == "1981-05-12");
  }

  TEST_CASE("CLI - credit arguments have exact arity and no punctuation codec", "[cli][integration][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg = idArg(fixture.addTrack(library::test::makeEmptyTrackSpec("credits.flac")));
    auto result = fixture.run(
      {"track",           "update",    trackArg,       "--credit", "soloist",  "  Glenn Gould  ", " Piano ",
       "--credit",        "conductor", "A=B: C, D; E", "",         "--credit", "performer",       "--title",
       "--clear-credits", "--credit",  "ensemble",     "--",       "-r",       "--title",         "Not consumed"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "updated 1 of 1 matched track(s)"));
    result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "Not consumed");
    auto const credits = tree.rootref()["credits"];
    REQUIRE(credits.num_children() == 4);
    CHECK(yaml::scalarView(credits[0]["name"]) == "A=B: C, D; E");
    CHECK(yaml::scalarView(credits[0]["kind"]) == "conductor");
    CHECK_FALSE(credits[0].has_child("role"));
    CHECK(yaml::scalarView(credits[1]["name"]) == "--");
    CHECK(yaml::scalarView(credits[1]["kind"]) == "ensemble");
    CHECK(yaml::scalarView(credits[1]["role"]) == "-r");
    CHECK(yaml::scalarView(credits[2]["name"]) == "Glenn Gould");
    CHECK(yaml::scalarView(credits[2]["role"]) == "Piano");
    CHECK(yaml::scalarView(credits[3]["name"]) == "--title");
    CHECK(yaml::scalarView(credits[3]["role"]) == "--clear-credits");

    result = fixture.run({"track",
                          "update",
                          trackArg,
                          "--credit",
                          "performer",
                          "A=B",
                          "A=B:C,;",
                          "--credit",
                          "performer",
                          "A=B",
                          "A=B:C,;"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "updated 1 of 1 matched track(s)"));
    result = fixture.run({"track",
                          "update",
                          trackArg,
                          "--credit",
                          "performer",
                          "A=B",
                          "A=B:C,;",
                          "--credit",
                          "performer",
                          "A=B",
                          "A=B:C,;"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "updated 0 of 1 matched track(s)"));
    result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    auto const replaced = tree.rootref()["credits"];
    REQUIRE(replaced.num_children() == 2);

    for (auto const entry : replaced.children())
    {
      CHECK(yaml::scalarView(entry["name"]) == "A=B");
      CHECK(yaml::scalarView(entry["kind"]) == "performer");
      CHECK(yaml::scalarView(entry["role"]) == "A=B:C,;");
    }
  }

  TEST_CASE("CLI - credit names and roles retain literal argv text", "[cli][integration][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg = idArg(fixture.addTrack(library::test::makeEmptyTrackSpec("literal-credits.flac")));

    for (auto const* literal : {"[unknown]",
                                "[traditional]",
                                "[Smith, John]",
                                "[]",
                                "[[aabb]]",
                                "A=B: C,D; E",
                                "--credit",
                                "--help",
                                "--title",
                                "--",
                                "-leading"})
    {
      CAPTURE(literal);
      auto result = fixture.run({"track",
                                 "update",
                                 trackArg,
                                 "--credit",
                                 "performer",
                                 literal,
                                 literal,
                                 "--credit",
                                 "performer",
                                 literal,
                                 "",
                                 "--credit",
                                 "performer",
                                 literal,
                                 literal,
                                 "--title",
                                 "Adjacent title"});
      INFO(result.err);
      REQUIRE(result.status == 0);
      CHECK(result.err.empty());
      result = fixture.run({"-O", "json", "track", "show", trackArg});
      REQUIRE(result.status == 0);
      auto tree = parseYaml(result.out);
      CHECK(yaml::scalarView(tree.rootref()["title"]) == "Adjacent title");
      auto const credits = tree.rootref()["credits"];
      REQUIRE(credits.num_children() == 3);

      for (auto const entry : credits.children())
      {
        CHECK(yaml::scalarView(entry["name"]) == literal);
        CHECK(yaml::scalarView(entry["kind"]) == "performer");
      }

      CHECK(yaml::scalarView(credits[0]["role"]) == literal);
      CHECK_FALSE(credits[1].has_child("role"));
      CHECK(yaml::scalarView(credits[2]["role"]) == literal);
    }
  }

  TEST_CASE("CLI - native argv credit role consumes dry-run literally while explicit preview remains safe",
            "[cli][integration][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg =
      idArg(fixture.addTrack(library::test::TrackSpec{.title = "Kept", .uri = "dry-run-credit.flac"}));
    auto const root = fixture.root().string();
    auto const argv = std::to_array<char const*>(
      {"aobus", "-C", root.c_str(), "track", "update", trackArg.c_str(), "--credit", "performer", "Name", "--dry-run"});
    auto out = std::ostringstream{};
    auto err = std::ostringstream{};
    // Invoke the actual argc/argv boundary: shells cannot distinguish a quoted
    // option-like role from an unquoted one once they produce these bytes.
    auto const status = run(static_cast<std::int32_t>(argv.size()),
                            argv.data(),
                            out,
                            err,
                            CliRunOptions{.musicLibraryPinnedMapBytes = library::test::kTestMusicLibraryMapBytes,
                                          .optCacheDirectory = fixture.cacheDirectory()});
    REQUIRE(status == 0);
    CHECK(err.str().empty());
    CHECK_FALSE(contains(out.str(), "(dry-run)"));
    auto result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    REQUIRE(tree.rootref()["credits"].num_children() == 1);
    CHECK(yaml::scalarView(tree.rootref()["credits"][0]["name"]) == "Name");
    CHECK(yaml::scalarView(tree.rootref()["credits"][0]["role"]) == "--dry-run");

    for (auto const& suffix :
         std::vector<std::vector<std::string>>{{"--dry-run", "--credit", "performer", "Preview only", ""},
                                               {"--credit", "performer", "Preview only", "", "--dry-run"}})
    {
      auto args =
        std::vector<std::string>{"aobus", "-C", root, "track", "update", trackArg, "--title", "Not persisted"};
      args.insert(args.end(), suffix.begin(), suffix.end());
      result = runArgs(args, fixture.cacheDirectory());
      REQUIRE(result.status == 0);
      CHECK(contains(result.out, "(dry-run)"));
      result = fixture.run({"-O", "json", "track", "show", trackArg});
      REQUIRE(result.status == 0);
      tree = parseYaml(result.out);
      CHECK(yaml::scalarView(tree.rootref()["title"]) == "Kept");
      REQUIRE(tree.rootref()["credits"].num_children() == 1);
      CHECK(yaml::scalarView(tree.rootref()["credits"][0]["name"]) == "Name");
      CHECK(yaml::scalarView(tree.rootref()["credits"][0]["role"]) == "--dry-run");
    }
  }

  TEST_CASE("CLI - every attached credit spelling rejects without metadata or tag mutation",
            "[cli][unit][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg =
      idArg(fixture.addTrack(library::test::TrackSpec{.title = "Kept",
                                                      .credits = {{.name = "Kept credit", .role = "Kept role"}},
                                                      .uri = "attached-credit.flac",
                                                      .tags = {"kept"}}));

    for (auto const* attached : {"--credit=performer",
                                 "--credit=false",
                                 "--credit=true",
                                 "--credit={}",
                                 "--credit=",
                                 "--credit=[]",
                                 "--credit=[[aabb]]",
                                 "--credit=[a,b]"})
    {
      CAPTURE(attached);
      auto const result = fixture.run(
        {"track", "update", trackArg, "--title", "Changed", "--add-tag", "staged", attached, "performer", "Name", ""});
      CHECK(result.status == static_cast<int>(CLI::ExitCodes::ArgumentMismatch));
      CHECK(result.out.empty());
      CHECK(contains(result.err, "--credit does not accept attached values"));
    }

    auto const result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "Kept");
    REQUIRE(tree.rootref()["credits"].num_children() == 1);
    CHECK(yaml::scalarView(tree.rootref()["credits"][0]["name"]) == "Kept credit");
    CHECK(yaml::scalarView(tree.rootref()["credits"][0]["role"]) == "Kept role");
    REQUIRE(tree.rootref()["tags"].num_children() == 1);
    CHECK(yaml::scalarView(tree.rootref()["tags"][0]) == "kept");
  }

  TEST_CASE("CLI - credit recognition respects ordinary option operands", "[cli][integration][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg = idArg(fixture.addTrack(library::test::makeEmptyTrackSpec("context-credits.flac")));
    auto result = fixture.run({"track",
                               "update",
                               trackArg,
                               "--title",
                               "--credit",
                               "--artist=--credit",
                               "--credit",
                               "performer",
                               "[unknown]",
                               "[]"});
    INFO(result.err);
    REQUIRE(result.status == 0);
    result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "--credit");
    CHECK(yaml::scalarView(tree.rootref()["artist"]) == "--credit");
    auto const credits = tree.rootref()["credits"];
    REQUIRE(credits.num_children() == 1);
    CHECK(yaml::scalarView(credits[0]["name"]) == "[unknown]");
    CHECK(yaml::scalarView(credits[0]["kind"]) == "performer");
    CHECK(yaml::scalarView(credits[0]["role"]) == "[]");

    // argv[0] is a program name, not an option. Root options still fall through
    // from track update after a literal triple has been consumed.
    result = runArgs({"--credit",
                      "track",
                      "update",
                      trackArg,
                      "--credit",
                      "soloist",
                      "[[aabb]]",
                      "",
                      "-C",
                      fixture.root().string(),
                      "-O",
                      "json",
                      "--title",
                      "After"},
                     fixture.cacheDirectory());
    INFO(result.err);
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "After");
    REQUIRE(tree.rootref()["credits"].num_children() == 1);
    CHECK(yaml::scalarView(tree.rootref()["credits"][0]["name"]) == "[[aabb]]");
    CHECK(yaml::scalarView(tree.rootref()["credits"][0]["kind"]) == "soloist");
    CHECK_FALSE(tree.rootref()["credits"][0].has_child("role"));

    result = fixture.run({"track", "update", trackArg, "--credit", "performer", "--", "--help", "--"});
    INFO(result.err);
    REQUIRE(result.status == 0);
    result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    REQUIRE(tree.rootref()["credits"].num_children() == 1);
    CHECK(yaml::scalarView(tree.rootref()["credits"][0]["name"]) == "--");
    CHECK(yaml::scalarView(tree.rootref()["credits"][0]["role"]) == "--help");
  }

  TEST_CASE("CLI - credit parsing preserves help and rejects wrong contexts without mutation",
            "[cli][integration][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg = idArg(fixture.addTrack(library::test::TrackSpec{
      .title = "Kept", .credits = {{.name = "Kept credit", .role = "Kept role"}}, .uri = "context-kept.flac"}));

    for (auto const& suffix : std::vector<std::vector<std::string>>{
           {"track", "update", trackArg, "--title", "Changed", "--credit", "performer", "Name", "", "--help"},
           {"track", "update", "--help", "--credit", "performer", "--help", "--credit"},
           // With its optional positionals satisfied, CLI11 returns from the
           // update subcommand at -- and recognizes the parent's help option.
           {"track", "update", trackArg, "--title", "Changed", "--credit", "performer", "Name", "", "--", "--help"},
           {"track", "show", "--filter", "--credit", "--help"},
           {"track", "create", "--help", "--", "--credit"},
           {"-C", "--credit", "track", "update", "--help"}})
    {
      auto args = std::vector<std::string>{"aobus", "-C", fixture.root().string()};
      // The last case supplies its own root value; duplicate scalar options
      // are outside this context test.
      if (suffix.front() == "-C")
      {
        args = {"aobus"};
      }

      args.insert(args.end(), suffix.begin(), suffix.end());
      auto const result = runArgs(args, fixture.cacheDirectory());
      CAPTURE(args, result.err);
      CHECK(result.status == 0);
      CHECK(result.err.empty());
      CHECK(contains(result.out, "OPTIONS:"));
      CHECK(contains(result.out, "Print this help message and exit"));
    }

    for (auto const& suffix : std::vector<std::vector<std::string>>{
           {"--credit", "performer", "Name", "", "track", "update", trackArg, "--title", "Changed"},
           {"track", "show", trackArg, "--credit", "performer", "Name", ""},
           {"track", "create", "--credit"},
           {"track", "update", "--", trackArg, "--credit", "performer", "Name", ""},
           {"track", "update", trackArg, "--title", "Changed", "--unknown", "--credit", "performer", "Name", ""}})
    {
      auto args = std::vector<std::string>{"aobus", "-C", fixture.root().string()};
      args.insert(args.end(), suffix.begin(), suffix.end());
      auto const result = runArgs(args, fixture.cacheDirectory());
      CAPTURE(args, result.err);
      CHECK(result.status != 0);
      CHECK_FALSE(result.err.empty());
    }

    auto const result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "Kept");
    auto const credits = tree.rootref()["credits"];
    REQUIRE(credits.num_children() == 1);
    CHECK(yaml::scalarView(credits[0]["name"]) == "Kept credit");
    CHECK(yaml::scalarView(credits[0]["role"]) == "Kept role");
  }

  TEST_CASE("CLI - incomplete credits and unknown metadata flags reject without mutation", "[cli][unit][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg = idArg(fixture.addTrack(library::test::TrackSpec{.title = "Kept", .uri = "kept.flac"}));

    for (auto const& suffix :
         std::vector<std::vector<std::string>>{{"--credit"},
                                               {"--credit", "soloist"},
                                               {"--credit", "soloist", "Name"},
                                               {"--credit", "performer", "[]"},
                                               {"--credit", "performer", "[[aabb]]"},
                                               {"--credit", "performer", "--help"},
                                               {"--credit", "soloist", "Name", "", "--credit", "performer", "Partial"},
                                               {"--musician", "Name"},
                                               {"--clear-musicians"},
                                               {"--conductor", "Name"},
                                               {"--ensemble", "Name"},
                                               {"--soloist", "Name"}})
    {
      auto args = std::vector<std::string>{
        "aobus", "-C", fixture.root().string(), "track", "update", trackArg, "--title", "Changed"};
      args.insert(args.end(), suffix.begin(), suffix.end());
      auto const result = runArgs(args, fixture.cacheDirectory());
      CHECK(result.status != 0);
      CHECK_FALSE(result.err.empty());

      if (suffix.front() == "--credit")
      {
        CHECK(result.status == static_cast<int>(CLI::ExitCodes::ArgumentMismatch));
      }
    }

    auto const result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "Kept");
    CHECK(tree.rootref()["credits"].num_children() == 0);
  }

  TEST_CASE("CLI - invalid credit text scopes and conflicting actions reject atomically", "[cli][unit][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg = idArg(fixture.addTrack(library::test::TrackSpec{
      .title = "Credited", .credits = {{.name = "Glenn Gould", .role = "piano"}}, .uri = "credited.flac"}));

    for (auto const& suffix : std::vector<std::vector<std::string>>{
           {"--credit", "performer", "", ""},
           {"--credit", "performer", " \t ", "Piano"},
           {"--credit", "performer", "Valid", "", "--credit", "soloist", " ", ""},
           {"--credit", "Soloist", "Name", ""},
           {"--credit", "musician", "Name", ""},
           {"--credit", "[performer]", "Name", ""},
           {"--credit", "--credit", "Name", ""},
           {"--credit", "--help", "Name", ""},
           {"--credit", "--title", "Name", ""},
           {"--credit", "--", "Name", ""},
           {"--credit", "", "Name", ""},
           {"--credit-scope", "soloist"},
           {"--credit-scope", "all", "--clear-credits"},
           {"--credit-scope", "conductor", "--credit", "soloist", "Name", ""},
           {"--credit", "performer", "Duo", "", "--clear-credits"},
           {"--credit", "performer", std::string(1, static_cast<char>(0xff)), ""},
           {"--credit", "performer", "Name", std::string(1, static_cast<char>(0xff))}})
    {
      auto args = std::vector<std::string>{
        "aobus", "-C", fixture.root().string(), "track", "update", trackArg, "--title", "Renamed"};
      args.insert(args.end(), suffix.begin(), suffix.end());
      auto const result = runArgs(args, fixture.cacheDirectory());
      CAPTURE(result.err);
      CHECK(result.status == 1);
      CHECK_FALSE(result.err.empty());
    }

    auto const result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "Credited");
    auto const credits = tree.rootref()["credits"];
    REQUIRE(credits.num_children() == 1);
    CHECK(yaml::scalarView(credits[0]["name"]) == "Glenn Gould");
    CHECK(yaml::scalarView(credits[0]["role"]) == "piano");
  }

  TEST_CASE("CLI - invalid credits reject even when the update filter matches no tracks", "[cli][unit][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg = idArg(fixture.addTrack(library::test::TrackSpec{.title = "Kept", .uri = "no-match.flac"}));
    auto const result = fixture.run(
      {"track", "update", "--filter", "$title = \"Missing\"", "--title", "Changed", "--credit", "performer", "", "[]"});
    checkDomainFailure(result, "invalid --credit value");
    auto const shown = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(shown.status == 0);
    auto tree = parseYaml(shown.out);
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "Kept");
    CHECK(tree.rootref()["credits"].num_children() == 0);
  }

  TEST_CASE("CLI - scope union replaces selected categories and preserves work and date",
            "[cli][integration][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg = idArg(fixture.addTrack(
      library::test::TrackSpec{.title = "Goldberg",
                               .composer = "J. S. Bach",
                               .work = "Goldberg Variations, BWV 988",
                               .movement = "Aria",
                               .recordingDate = date(1981),
                               .credits = {{.name = "Leonard Bernstein", .kind = library::CreditKind::Conductor},
                                           {.name = "Berliner Philharmoniker", .kind = library::CreditKind::Ensemble},
                                           {.name = "Glenn Gould", .kind = library::CreditKind::Soloist},
                                           {.name = "Glenn Gould", .role = "piano"}},
                               .uri = "goldberg.flac",
                               .movementNumber = 1,
                               .movementTotal = 32}));

    auto result = fixture.run({"track",
                               "update",
                               trackArg,
                               "--credit-scope",
                               "performer",
                               "--credit-scope",
                               "soloist",
                               "--credit-scope",
                               "performer",
                               "--credit",
                               "performer",
                               "Alfred Brendel",
                               "Piano"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "updated 1 of 1 matched track(s)"));
    result = fixture.run({"track", "update", trackArg, "--recording-date", "1955"});
    REQUIRE(result.status == 0);
    result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    auto root = tree.rootref();
    CHECK(yaml::scalarView(root["composer"]) == "J. S. Bach");
    CHECK(yaml::scalarView(root["work"]) == "Goldberg Variations, BWV 988");
    CHECK(yaml::scalarView(root["movement"]) == "Aria");
    CHECK(yaml::scalarView(root["movementNumber"]) == "1");
    CHECK(yaml::scalarView(root["movementTotal"]) == "32");
    CHECK(yaml::scalarView(root["recordingDate"]) == "1955");
    auto const credits = root["credits"];
    REQUIRE(credits.num_children() == 3);
    CHECK(yaml::scalarView(credits[0]["name"]) == "Leonard Bernstein");
    CHECK(yaml::scalarView(credits[1]["name"]) == "Berliner Philharmoniker");
    CHECK(yaml::scalarView(credits[2]["name"]) == "Alfred Brendel");
    CHECK(yaml::scalarView(credits[2]["kind"]) == "performer");
    CHECK(yaml::scalarView(credits[2]["role"]) == "Piano");

    result = fixture.run({"track", "update", trackArg, "--credit-scope", "performer", "--clear-credits"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "updated 1 of 1 matched track(s)"));
    result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    root = tree.rootref();
    REQUIRE(root["credits"].num_children() == 2);
    CHECK(yaml::scalarView(root["credits"][0]["name"]) == "Leonard Bernstein");
    CHECK(yaml::scalarView(root["credits"][1]["name"]) == "Berliner Philharmoniker");
    CHECK(yaml::scalarView(root["recordingDate"]) == "1955");

    result = fixture.run({"track", "update", trackArg, "--clear-credits"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "updated 1 of 1 matched track(s)"));
    result = fixture.run({"track", "update", trackArg, "--clear-credits"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "updated 0 of 1 matched track(s)"));
    result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(tree.rootref()["credits"].num_children() == 0);
    CHECK(yaml::scalarView(tree.rootref()["recordingDate"]) == "1955");
  }

  TEST_CASE("CLI - structured update reports carry performance metadata changes", "[cli][integration][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg = idArg(fixture.addTrack(library::test::makeEmptyTrackSpec("report.flac")));
    auto result = fixture.run({"-O", "json", "track", "update", trackArg, "--recording-date", "1981-05"});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    auto const dateChange = tree.rootref()["changes"][0]["fields"][0];
    CHECK(yaml::scalarView(dateChange["field"]) == "recordingDate");
    CHECK(yaml::scalarView(dateChange["oldValue"]).empty());
    CHECK(yaml::scalarView(dateChange["newValue"]) == "1981-05");

    result = fixture.run({"-O", "json", "track", "update", trackArg, "--credit", "performer", "Glenn Gould", "Piano"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    auto const creditChange = tree.rootref()["changes"][0]["fields"][0];
    CHECK(yaml::scalarView(creditChange["field"]) == "credits");
    CHECK(yaml::scalarView(creditChange["oldValue"]).empty());
    auto const diagnostic = std::string{yaml::scalarView(creditChange["newValue"])};
    CHECK(diagnostic == "performer: Glenn Gould (Piano)");

    result = fixture.run({"-O", "json", "track", "update", trackArg, "--clear-credits"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    auto const clearChange = tree.rootref()["changes"][0]["fields"][0];
    CHECK(yaml::scalarView(clearChange["field"]) == "credits");
    CHECK(yaml::scalarView(clearChange["oldValue"]) == diagnostic);
    CHECK(yaml::scalarView(clearChange["newValue"]).empty());
  }

  TEST_CASE("CLI - track update reserves built-in metadata keys for --set", "[cli][unit][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg = idArg(fixture.addTrack(library::test::makeEmptyTrackSpec("reserved.flac")));
    checkDomainFailure(fixture.run({"track", "update", trackArg, "--set", "credits=Glenn Gould"}),
                       "invalid --set key 'credits': reserved metadata key");

    for (auto const& definition : rt::trackFieldDefinitions())
    {
      checkDomainFailure(fixture.run({"track", "update", trackArg, "--set", std::string{definition.id} + "=X"}),
                         "reserved metadata key");
    }

    checkDomainFailure(
      fixture.run({"track", "update", trackArg, "--title", "Renamed", "--set", "credits=X"}), "reserved metadata key");
    auto result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK_FALSE(tree.rootref()["title"].readable());
    CHECK(tree.rootref()["credits"].num_children() == 0);
    auto const customNode = tree.rootref()["custom"];
    CHECK(customNode.readable());
    CHECK(customNode.is_map());
    CHECK(customNode.num_children() == 0);

    result = fixture.run({"track",
                          "update",
                          trackArg,
                          "--set",
                          "source=manual",
                          "--set",
                          "Title=not-a-field",
                          "--set",
                          "CREDITS=not-a-list",
                          "--set",
                          "albumArtist=query-name",
                          "--set",
                          "recordingDate=query-date",
                          "--set",
                          "trackNumber=query-number",
                          "--set",
                          "t=alias",
                          "--set",
                          "musicians=retired",
                          "--set",
                          "credit=singular"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "updated 1 of 1 matched track(s)"));
    result = fixture.run({"-O", "json", "track", "show", trackArg});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    auto const custom = tree.rootref()["custom"];
    REQUIRE(custom.is_map());
    CHECK(yaml::scalarView(custom["source"]) == "manual");
    CHECK(yaml::scalarView(custom["Title"]) == "not-a-field");
    CHECK(yaml::scalarView(custom["CREDITS"]) == "not-a-list");
    CHECK(yaml::scalarView(custom["albumArtist"]) == "query-name");
    CHECK(yaml::scalarView(custom["recordingDate"]) == "query-date");
    CHECK(yaml::scalarView(custom["trackNumber"]) == "query-number");
    CHECK(yaml::scalarView(custom["t"]) == "alias");
    CHECK(yaml::scalarView(custom["musicians"]) == "retired");
    CHECK(yaml::scalarView(custom["credit"]) == "singular");
    CHECK_FALSE(custom.has_child("title"));
    CHECK_FALSE(custom.has_child("credits"));
  }

  TEST_CASE("CLI - track update help documents performance metadata options", "[cli][unit][track][help]")
  {
    auto fixture = CliFixture{};
    auto const result = fixture.run({"track", "update", "--help"});
    INFO(result.out);
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "--recording-date"));
    CHECK(contains(result.out, "YYYY, YYYY-MM, or YYYY-MM-DD"));
    CHECK(contains(result.out, "--credit"));
    CHECK(contains(result.out, "KIND NAME ROLE"));
    CHECK(contains(result.out, "--credit-scope"));
    CHECK(contains(result.out, "--clear-credits"));
    // CLI11 wraps footer text to the native terminal width.
    auto words = std::istringstream{result.out};
    auto normalizedHelp = std::string{};

    for (auto word = std::string{}; words >> word;)
    {
      if (!normalizedHelp.empty())
      {
        normalizedHelp.push_back(' ');
      }

      normalizedHelp += word;
    }

    CHECK(contains(normalizedHelp, "--recording-date 1981-05-12 --credit soloist 'Glenn Gould' Piano"));
    CHECK(contains(normalizedHelp, "--dry-run in NAME or ROLE is literal text, not a preview flag"));
    CHECK(contains(normalizedHelp, "--dry-run --credit performer 'Preview only' ''"));
    CHECK(contains(normalizedHelp, "Attached --credit=VALUE syntax is not supported"));
    CHECK_FALSE(contains(result.out, "--musician"));
    CHECK_FALSE(contains(result.out, "--conductor"));
    CHECK_FALSE(contains(result.out, "--ensemble"));
    CHECK_FALSE(contains(result.out, "--soloist"));
  }
} // namespace ao::cli::test
