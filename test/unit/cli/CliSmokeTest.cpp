// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliTestSupport.h"
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::cli::test
{
  TEST_CASE("CLI - init and dump commands run against fixture library", "[cli][integration][command-dispatch]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");
    fixture.copyAudio("hires.flac", "hires.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "new 2  changed 0  moved 0  missing 0  unchanged 0  errors 0"));

    result = fixture.run({"init"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "new 0  changed 0  moved 0  missing 0  unchanged 2  errors 0"));

    result = fixture.run({"-O", "json", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(countOccurrences(result.out, R"("title":)") == 2);

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Test Title"));
    CHECK(contains(result.out, "HiRes Title"));

    result = fixture.run({"-O", "yaml", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "tracks:"));
    CHECK(contains(result.out, "artist: \"Test Artist\""));
    CHECK(countOccurrences(result.out, "id:") == 2);

    result = fixture.run({"-O", "yaml", "track", "show", "--limit", "1"});
    REQUIRE(result.status == 0);
    CHECK(countOccurrences(result.out, "id:") == 1);
    auto const firstPage = result.out;

    result = fixture.run({"-O", "yaml", "track", "show", "--offset", "1"});
    REQUIRE(result.status == 0);
    CHECK(countOccurrences(result.out, "id:") == 1);
    CHECK(result.out != firstPage);

    result = fixture.run({"-O", "json", "track", "show", "--filter", "$title ~ \"Test\""});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "Test Title");

    result = fixture.run({"track", "show", "--filter", "#NonExistentTag"});
    REQUIRE(result.status == 0);
    CHECK(result.out.empty());
    CHECK(result.err.empty());

    result = fixture.run({"track", "dump", "--id", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Title:"));

    result = fixture.run({"track", "dump", "--raw"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Track ID:"));

    result = fixture.run({"-O", "yaml", "list", "dump"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "lists:"));

    result = fixture.run({"-O", "yaml", "lib", "dump", "--meta"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "meta:"));

    result = fixture.run({"lib", "dump", "--dict"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Dictionary"));

    result = fixture.run({"-O", "yaml", "lib", "dump", "--manifest"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "manifest:"));

    result = fixture.run({"lib", "dump", "--resources"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Resources"));

    result = fixture.run({"-O", "json", "lib", "dump", "--manifest"});
    REQUIRE(result.status == 0);
    auto jsonTree = parseYaml(result.out);
    REQUIRE(jsonTree.rootref().is_map());
    CHECK(jsonTree.rootref()["manifest"].is_seq());
  }

  TEST_CASE("CLI - mutation summaries honor structured output", "[cli][integration][output]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"-O", "json", "init"});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    auto initTree = parseYaml(result.out);
    CHECK(yaml::scalarView(initTree.rootref()["new"]) == "1");
    CHECK(yaml::scalarView(initTree.rootref()["dryRun"]) == "false");

    result = fixture.run({"-O", "json", "list", "create", "--name", "Machine"});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    auto createTree = parseYaml(result.out);
    CHECK(yaml::scalarView(createTree.rootref()["action"]) == "create");
    CHECK(yaml::scalarView(createTree.rootref()["name"]) == "Machine");

    result = fixture.run({"-O", "yaml", "track", "delete", "1"});
    REQUIRE(result.status == 0);
    auto deleteTree = parseYaml(result.out);
    CHECK(yaml::scalarView(deleteTree.rootref()["action"]) == "delete");
    CHECK(yaml::scalarView(deleteTree.rootref()["trackId"]) == "1");
  }

  TEST_CASE("CLI - empty YAML collections are sequences", "[cli][unit][output]")
  {
    auto fixture = CliFixture{};

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "yaml", "list", "show"});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    REQUIRE(tree.rootref()["lists"].is_seq());
    CHECK(tree.rootref()["lists"].num_children() == 0);

    result = fixture.run({"-O", "yaml", "lib", "resource", "list"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    REQUIRE(tree.rootref()["resources"].is_seq());
    CHECK(tree.rootref()["resources"].num_children() == 0);

    result = fixture.run({"-O", "yaml", "lib", "dump", "--manifest"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    REQUIRE(tree.rootref()["manifest"].is_seq());
    CHECK(tree.rootref()["manifest"].num_children() == 0);

    result = fixture.run({"-O", "yaml", "lib", "dump", "--resources"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    REQUIRE(tree.rootref()["resources"].is_seq());
    CHECK(tree.rootref()["resources"].num_children() == 0);
  }
} // namespace ao::cli::test
