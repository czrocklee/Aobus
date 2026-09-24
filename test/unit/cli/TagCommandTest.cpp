// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliTestSupport.h"
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace ao::cli::test
{
  TEST_CASE("CLI - tag commands mutate track tags", "[cli][integration][tag]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"tag", "add", "fav", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "added tag: fav to 1 track(s)"));

    result = fixture.run({"tag", "show", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "fav"));

    result = fixture.run({"-O", "json", "tag", "show", "1"});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    auto tree = parseYaml(result.out);
    REQUIRE(tree.rootref()["tags"].is_seq());
    REQUIRE(tree.rootref()["tags"].num_children() == 1);
    CHECK(yaml::scalarView(tree.rootref()["tags"][0]) == "fav");

    result = fixture.run({"tag", "remove", "fav", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "removed tag: fav from 1 track(s)"));

    result = fixture.run({"tag", "show", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "no tags"));

    result = fixture.run({"tag", "remove", "missing", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "removed tag: missing from 0 track(s)"));
  }

  TEST_CASE("CLI - tag list and batch targets use reader and writer contracts", "[cli][integration][tag]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");
    fixture.copyAudio("hires.flac", "hires.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"tag", "add", "fav", "1", "2"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "added tag: fav to 2 track(s)"));

    result = fixture.run({"tag", "add", "chill", "1"});
    REQUIRE(result.status == 0);

    result = fixture.run({"tag", "list"});
    REQUIRE(result.status == 0);
    auto const favOffset = result.out.find("fav  2");
    auto const chillOffset = result.out.find("chill  1");
    REQUIRE(favOffset != std::string::npos);
    REQUIRE(chillOffset != std::string::npos);
    CHECK(favOffset < chillOffset);

    result = fixture.run({"tag", "show", "1", "2"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "fav"));
    CHECK_FALSE(contains(result.out, "chill"));

    result = fixture.run({"tag", "add", "live", "--filter", "$title ~ \"Title\""});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "added tag: live to 2 track(s)"));

    result = fixture.run({"-O", "json", "tag", "list"});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    bool foundFav = false;
    bool foundLive = false;

    for (auto const tagNode : tree.rootref()["tags"].children())
    {
      if (yaml::scalarView(tagNode["name"]) == "fav" && yaml::scalarView(tagNode["count"]) == "2")
      {
        foundFav = true;
      }
      else if (yaml::scalarView(tagNode["name"]) == "live" && yaml::scalarView(tagNode["count"]) == "2")
      {
        foundLive = true;
      }
    }

    CHECK(foundFav);
    CHECK(foundLive);
  }

  TEST_CASE("CLI - tag mutations support dry-run reports", "[cli][integration][tag][dry-run]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "json", "tag", "add", "--dry-run", "Favorite", "1"});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["changes"][0]["addedTags"][0]) == "Favorite");

    result = fixture.run({"tag", "show", "1"});
    REQUIRE(result.status == 0);
    CHECK_FALSE(contains(result.out, "Favorite"));

    result = fixture.run({"-O", "json", "tag", "add", "Favorite", "1"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");

    result = fixture.run({"tag", "show", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Favorite"));

    result = fixture.run({"-O", "json", "tag", "remove", "--dry-run", "Favorite", "1"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["changes"][0]["removedTags"][0]) == "Favorite");

    result = fixture.run({"tag", "show", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Favorite"));

    result = fixture.run({"-O", "json", "tag", "remove", "Favorite", "1"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");

    result = fixture.run({"tag", "show", "1"});
    REQUIRE(result.status == 0);
    CHECK_FALSE(contains(result.out, "Favorite"));
  }

  TEST_CASE("CLI - tag domain failures use stderr and exit non-zero", "[cli][unit][tag][contract]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    checkDomainFailure(fixture.run({"tag", "add", "fav", "999"}), "track not found: 999");
    checkDomainFailure(fixture.run({"tag", "add", "fav"}), "tag command requires track ids");
    checkDomainFailure(fixture.run({"tag", "add", "fav", "--filter", "("}), "filter error:");
    checkDomainFailure(fixture.run({"tag", "show", "999"}), "track not found: 999");
  }
} // namespace ao::cli::test
