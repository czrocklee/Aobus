// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CliTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/yaml/RymlAdapter.h>

#include <CLI/Error.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace ao::cli::test
{
  namespace
  {
    ryml::Tree showLiteralTrack(CliFixture const& fixture, std::string const& trackArg)
    {
      auto const result = fixture.run({"-O", "json", "track", "show", trackArg});
      REQUIRE(result.status == 0);
      CHECK(result.err.empty());
      return parseYaml(result.out);
    }

    std::vector<std::string> literalTags(ryml::ConstNodeRef track)
    {
      auto tags = std::vector<std::string>{};

      for (auto const tag : track["tags"].children())
      {
        tags.emplace_back(yaml::scalarView(tag));
      }

      std::ranges::sort(tags);
      return tags;
    }
  } // namespace

  TEST_CASE("CLI - tag and custom operands preserve literal brackets and commas", "[cli][integration][track][update]")
  {
    for (auto const* literal : {"[live]", "[]", "[[aabb]]", "[Smith, John]", "a,b"})
    {
      CAPTURE(literal);
      auto fixture = CliFixture{};
      auto const trackArg = std::to_string(fixture.addTrack(library::test::makeEmptyTrackSpec("literal.flac")).raw());
      auto const assignment = std::string{literal} + "=" + literal;
      auto result =
        fixture.run({"track", "update", trackArg, "--add-tag", literal, "--set", assignment, "--title", "After"});
      REQUIRE(result.status == 0);
      CHECK(result.err.empty());
      auto tree = showLiteralTrack(fixture, trackArg);
      CHECK(yaml::scalarView(tree.rootref()["title"]) == "After");
      CHECK(literalTags(tree.rootref()) == std::vector<std::string>{literal});
      REQUIRE(tree.rootref()["custom"].num_children() == 1);
      CHECK(yaml::scalarView(tree.rootref()["custom"][literal]) == literal);

      result = fixture.run({"track", "update", trackArg, "--remove-tag", literal, "--unset", literal});
      REQUIRE(result.status == 0);
      tree = showLiteralTrack(fixture, trackArg);
      CHECK(literalTags(tree.rootref()).empty());
      CHECK(tree.rootref()["custom"].num_children() == 0);
      CHECK(yaml::scalarView(tree.rootref()["title"]) == "After");
    }
  }

  TEST_CASE("CLI - literal vector repeats attached forms and multiple operands retain order",
            "[cli][integration][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg = std::to_string(fixture.addTrack(library::test::makeEmptyTrackSpec("vectors.flac")).raw());
    auto result = fixture.run({"track",
                               "update",
                               trackArg,
                               "--set",
                               "[mood]=bracket",
                               "mood=plain",
                               "[]=empty-brackets",
                               "[[aabb]]=double",
                               "comma,key=comma,value",
                               "--set=[source=manual]",
                               "--set",
                               "order=first",
                               "order=second",
                               "--set=order=last",
                               "--add-tag",
                               "live",
                               "[live]",
                               "[]",
                               "[[aabb]]",
                               "a,b",
                               "--add-tag=[source=manual]",
                               "--title",
                               "After"});
    REQUIRE(result.status == 0);
    auto tree = showLiteralTrack(fixture, trackArg);
    auto custom = tree.rootref()["custom"];
    CHECK(yaml::scalarView(custom["[mood]"]) == "bracket");
    CHECK(yaml::scalarView(custom["mood"]) == "plain");
    CHECK(yaml::scalarView(custom["[]"]) == "empty-brackets");
    CHECK(yaml::scalarView(custom["[[aabb]]"]) == "double");
    CHECK(yaml::scalarView(custom["comma,key"]) == "comma,value");
    CHECK(yaml::scalarView(custom["[source"]) == "manual]");
    CHECK_FALSE(custom.has_child("source"));
    CHECK(yaml::scalarView(custom["order"]) == "last");
    CHECK(literalTags(tree.rootref()) ==
          std::vector<std::string>{"[[aabb]]", "[]", "[live]", "[source=manual]", "a,b", "live"});

    // Plain and bracketed identities coexist: deletion must not hit the plain
    // key/tag that CLI11's vector codec formerly substituted.
    result = fixture.run({"track",
                          "update",
                          trackArg,
                          "--unset=[mood]",
                          "--unset",
                          "[]",
                          "[[aabb]]",
                          "--remove-tag=[live]",
                          "--remove-tag",
                          "[]",
                          "[[aabb]]",
                          "--title",
                          "Still after"});
    REQUIRE(result.status == 0);
    tree = showLiteralTrack(fixture, trackArg);
    custom = tree.rootref()["custom"];
    CHECK_FALSE(custom.has_child("[mood]"));
    CHECK_FALSE(custom.has_child("[]"));
    CHECK_FALSE(custom.has_child("[[aabb]]"));
    CHECK(yaml::scalarView(custom["mood"]) == "plain");
    CHECK(yaml::scalarView(custom["comma,key"]) == "comma,value");
    CHECK(yaml::scalarView(custom["order"]) == "last");
    CHECK(literalTags(tree.rootref()) == std::vector<std::string>{"[source=manual]", "a,b", "live"});
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "Still after");
  }

  TEST_CASE("CLI - attached literal values are not boolean flag overrides", "[cli][integration][track][update]")
  {
    for (auto const* literal : {"false", "true", "{}", "[]", "[[aabb]]", "[a,b]"})
    {
      CAPTURE(literal);
      auto fixture = CliFixture{};
      auto const trackArg = std::to_string(fixture.addTrack(library::test::makeEmptyTrackSpec("attached.flac")).raw());
      auto result = fixture.run({"track",
                                 "update",
                                 trackArg,
                                 std::string{"--add-tag="} + literal,
                                 std::string{"--set="} + literal + "=" + literal});
      REQUIRE(result.status == 0);
      auto tree = showLiteralTrack(fixture, trackArg);
      CHECK(literalTags(tree.rootref()) == std::vector<std::string>{literal});
      CHECK(yaml::scalarView(tree.rootref()["custom"][literal]) == literal);
      result = fixture.run(
        {"track", "update", trackArg, std::string{"--remove-tag="} + literal, std::string{"--unset="} + literal});
      REQUIRE(result.status == 0);
      tree = showLiteralTrack(fixture, trackArg);
      CHECK(literalTags(tree.rootref()).empty());
      CHECK(tree.rootref()["custom"].num_children() == 0);
    }
  }

  TEST_CASE("CLI - literal vectors use native short root numeric and delimiter context",
            "[cli][integration][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg =
      std::to_string(fixture.addTrack(library::test::TrackSpec{.title = "Before", .uri = "context.flac"}).raw());
    auto const result = runArgs({"--add-tag",
                                 "track",
                                 "update",
                                 trackArg,
                                 "--title",
                                 "--set",
                                 "--artist=--unset",
                                 "--set",
                                 "context=first",
                                 "--",
                                 "--add-tag",
                                 "-leading",
                                 "-12",
                                 "-.5",
                                 "[live]",
                                 "-Ojson",
                                 "--set=context=last",
                                 "-C" + fixture.root().string()},
                                fixture.cacheDirectory());
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    auto tree = showLiteralTrack(fixture, trackArg);
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "--set");
    CHECK(yaml::scalarView(tree.rootref()["artist"]) == "--unset");
    CHECK(yaml::scalarView(tree.rootref()["custom"]["context"]) == "last");
    CHECK(literalTags(tree.rootref()) == std::vector<std::string>{"-.5", "-12", "-leading", "[live]"});

    // The short filter consumes its joined operand before the next long raw
    // callback. A vector delimiter resumes option parsing, unlike credit data.
    auto filtered =
      fixture.run({"-Ojson", "track.update", "-f$title = \"--set\"", "--set=filtered=[]", "--", "--title", "Filtered"});
    REQUIRE(filtered.status == 0);
    tree = showLiteralTrack(fixture, trackArg);
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "Filtered");
    CHECK(yaml::scalarView(tree.rootref()["custom"]["filtered"]) == "[]");

    filtered = fixture.run(
      {"track", "update", trackArg, "--add-tag", "--help", "--add-tag=--unknown", "--title", "Literal flags"});
    REQUIRE(filtered.status == 0);
    tree = showLiteralTrack(fixture, trackArg);
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "Literal flags");
    CHECK(literalTags(tree.rootref()) ==
          std::vector<std::string>{"--help", "--unknown", "-.5", "-12", "-leading", "[live]"});
  }

  TEST_CASE("CLI - native dotted options and synthetic long remainders retain raw operand context",
            "[cli][integration][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg =
      std::to_string(fixture.addTrack(library::test::TrackSpec{.title = "Dotted", .uri = "dotted.flac"}).raw());
    // Select the leaf command before root-qualified options fall through to native dot rewriting.
    auto const result = fixture.run({"track",
                                     "update",
                                     "--track.update.filter=$title = \"Dotted\"",
                                     "--track.update.set=source=[]",
                                     "--track.update.add-tag=[dotted]"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    auto tree = showLiteralTrack(fixture, trackArg);
    REQUIRE(tree.rootref().has_child("custom"));
    REQUIRE(tree.rootref()["custom"].has_child("source"));
    CHECK(yaml::scalarView(tree.rootref()["custom"]["source"]) == "[]");
    CHECK(literalTags(tree.rootref()) == std::vector<std::string>{"[dotted]"});
    auto const help = fixture.run({"-h-track.update.set=from-short=[]"});
    CHECK(help.status == 0);
    CHECK(contains(help.out, "OPTIONS:"));
    tree = showLiteralTrack(fixture, trackArg);
    CHECK_FALSE(tree.rootref()["custom"].has_child("from-short"));
  }

  TEST_CASE("CLI - attached literal operands follow multiple positional targets", "[cli][integration][track][update]")
  {
    auto fixture = CliFixture{};
    auto const firstArg =
      std::to_string(fixture.addTrack(library::test::TrackSpec{.title = "First", .uri = "first.flac"}).raw());
    auto const secondArg =
      std::to_string(fixture.addTrack(library::test::TrackSpec{.title = "Second", .uri = "second.flac"}).raw());
    auto const result = fixture.run({"-Ojson",
                                     "track",
                                     "update",
                                     firstArg,
                                     secondArg,
                                     "--set=source=[first, second]",
                                     "--add-tag=[live]",
                                     "--title",
                                     "After"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());

    for (auto const& trackArg : {firstArg, secondArg})
    {
      auto tree = showLiteralTrack(fixture, trackArg);
      CHECK(yaml::scalarView(tree.rootref()["title"]) == "After");
      CHECK(yaml::scalarView(tree.rootref()["custom"]["source"]) == "[first, second]");
      CHECK(literalTags(tree.rootref()) == std::vector<std::string>{"[live]"});
    }
  }

  TEST_CASE("CLI - missing empty and invalid literal operands reject all metadata and tag edits",
            "[cli][unit][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg = std::to_string(
      fixture.addTrack(library::test::TrackSpec{.title = "Kept", .uri = "kept.flac", .tags = {"kept"}}).raw());

    for (auto const* option : {"--set", "--unset", "--add-tag", "--remove-tag"})
    {
      CAPTURE(option);
      auto result = fixture.run({"track", "update", trackArg, "--title", "Changed", "--add-tag", "staged", option});
      CHECK(result.status == static_cast<int>(CLI::ExitCodes::ArgumentMismatch));
      CHECK(result.out.empty());
      CHECK_FALSE(result.err.empty());

      if (std::string_view{option} == "--set" || std::string_view{option} == "--unset")
      {
        result = fixture.run({"track", "update", trackArg, "--title", "Changed", "--add-tag", "staged", option, ""});
        CHECK(result.status == 1);
        CHECK(result.out.empty());
        result = fixture.run(
          {"track", "update", trackArg, "--title", "Changed", "--add-tag", "staged", std::string{option} + "="});
        CHECK(result.status == 1);
        CHECK(result.out.empty());
      }
    }

    for (auto const& suffix :
         std::vector<std::vector<std::string>>{{"--set", "missing-equals"},
                                               {"--set", "=value"},
                                               {"--set", "credits=reserved"},
                                               {"--set", std::string{"key="} + static_cast<char>(0xff)},
                                               {"--unset", std::string(1, static_cast<char>(0xff))},
                                               {"--add-tag", std::string(1, static_cast<char>(0xff))},
                                               {"--remove-tag", std::string(1, static_cast<char>(0xff))},
                                               {"--add-tag", "[live]", "--remove-tag", "[live]"},
                                               {"--add-tag", "[live]", "--dry-run"}})
    {
      auto args = std::vector<std::string>{"aobus",
                                           "-C",
                                           fixture.root().string(),
                                           "track",
                                           "update",
                                           trackArg,
                                           "--title",
                                           "Changed",
                                           "--set",
                                           "staged=value",
                                           "--add-tag",
                                           "staged"};
      args.insert(args.end(), suffix.begin(), suffix.end());
      auto const result = runArgs(args, fixture.cacheDirectory());
      CAPTURE(args, result.err);
      CHECK(result.status == 1);
      CHECK(result.out.empty());
    }

    auto tree = showLiteralTrack(fixture, trackArg);
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "Kept");
    CHECK(tree.rootref()["custom"].num_children() == 0);
    CHECK(literalTags(tree.rootref()) == std::vector<std::string>{"kept"});
  }

  TEST_CASE("CLI - empty literal tag operands and custom values retain their existing meaning",
            "[cli][integration][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg = std::to_string(fixture.addTrack(library::test::makeEmptyTrackSpec("empty.flac")).raw());
    auto result =
      fixture.run({"track", "update", trackArg, "--add-tag", "", "--set", "empty=", "equals=a=b", "--title", "After"});
    REQUIRE(result.status == 0);
    auto tree = showLiteralTrack(fixture, trackArg);
    CHECK(literalTags(tree.rootref()) == std::vector<std::string>{""});
    REQUIRE(tree.rootref()["custom"].has_child("empty"));
    CHECK(yaml::scalarView(tree.rootref()["custom"]["empty"]).empty());
    CHECK(yaml::scalarView(tree.rootref()["custom"]["equals"]) == "a=b");
    result = fixture.run({"track", "update", trackArg, "--remove-tag=", "--title", "Removed"});
    REQUIRE(result.status == 0);
    tree = showLiteralTrack(fixture, trackArg);
    CHECK(literalTags(tree.rootref()).empty());
    result = fixture.run({"track", "update", trackArg, "--add-tag=", "--set=empty=", "--title", "Attached"});
    REQUIRE(result.status == 0);
    tree = showLiteralTrack(fixture, trackArg);
    CHECK(literalTags(tree.rootref()) == std::vector<std::string>{""});
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "Attached");
    result = fixture.run({"track", "update", trackArg, "--remove-tag", ""});
    REQUIRE(result.status == 0);
    tree = showLiteralTrack(fixture, trackArg);
    CHECK(literalTags(tree.rootref()).empty());
    CHECK(yaml::scalarView(tree.rootref()["custom"]["equals"]) == "a=b");
  }

  TEST_CASE("CLI - literal option recognition preserves help unknown and no-target validation",
            "[cli][unit][track][update]")
  {
    auto fixture = CliFixture{};
    auto const trackArg =
      std::to_string(fixture.addTrack(library::test::TrackSpec{.title = "Kept", .uri = "help.flac"}).raw());

    for (auto const& tail : std::vector<std::vector<std::string>>{
           {"--help"}, {"-hOjson", "--set=after-short=[]"}, {"--help-all"}, {"--unknown"}, {"-z"}, {"++", "--unknown"}})
    {
      auto args = std::vector<std::string>{
        "aobus", "-C", fixture.root().string(), "track", "update", trackArg, "--set", "key=[]", "--add-tag", "[live]"};
      args.insert(args.end(), tail.begin(), tail.end());
      auto const result = runArgs(args, fixture.cacheDirectory());
      CAPTURE(args, result.err);

      if (tail.front() == "--help" || tail.front() == "-hOjson" || tail.front() == "--help-all")
      {
        CHECK(result.status == 0);
        CHECK(contains(result.out, "OPTIONS:"));
      }
      else
      {
        CHECK(result.status != 0);
        CHECK_FALSE(result.err.empty());
      }
    }

    checkDomainFailure(
      fixture.run({"track", "update", "--filter", "$title = \"Missing\"", "--set", "invalid"}), "invalid --set value");
    checkDomainFailure(
      fixture.run({"track", "update", "--filter", "$title = \"Missing\"", "--unset="}), "invalid --unset value");
    auto const noTargets =
      fixture.run({"track", "update", "--filter", "$title = \"Missing\"", "--set=key=[]", "--add-tag=[live]"});
    CHECK(noTargets.status == 0);
    CHECK(contains(noTargets.out, "updated 0 of 0 matched track(s)"));
    auto const wrongContext = fixture.run({"track", "show", trackArg, "--set", "key=value"});
    CHECK(wrongContext.status != 0);
    auto const delimited = fixture.run({"track", "update", "--", trackArg, "--set", "key=value"});
    CHECK(delimited.status != 0);
    auto tree = showLiteralTrack(fixture, trackArg);
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "Kept");
    CHECK(tree.rootref()["custom"].num_children() == 0);
    CHECK(literalTags(tree.rootref()).empty());
  }
} // namespace ao::cli::test
