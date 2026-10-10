// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/yaml/RymlAdapter.h>

#include <CLI/Error.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <string>
#include <string_view>

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

    result = fixture.run({"tag", "add", "smoke", "--filter", "$title ~ \"Test\""});
    REQUIRE(result.status == 0);

    // tag show accepts the same target selection as tag add and tag remove.
    result = fixture.run({"tag", "show", "--filter", "$title ~ \"Test\""});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "smoke"));

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
    auto const listId = std::string{yaml::scalarView(createTree.rootref()["listId"])};

    // The detail document wraps one row in `list`, in JSON as in YAML.
    result = fixture.run({"-O", "json", "list", "show", listId});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    auto showTree = parseYaml(result.out);
    CHECK(yaml::scalarView(showTree.rootref()["list"]["name"]) == "Machine");

    result = fixture.run({"-O", "json", "track", "update", "1", "--title", "Renamed", "--add-tag", "fav"});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    auto updateTree = parseYaml(result.out);
    CHECK(yaml::scalarView(updateTree.rootref()["updated"]) == "1");
    CHECK(yaml::scalarView(updateTree.rootref()["tagChanges"][0]["addedTags"][0]) == "fav");

    result = fixture.run({"-O", "json", "track", "update", "1", "--set=source=[manual]", "--add-tag=[literal]"});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    auto literalUpdate = parseYaml(result.out);
    CHECK(yaml::scalarView(literalUpdate.rootref()["updated"]) == "1");
    CHECK(yaml::scalarView(literalUpdate.rootref()["tagChanges"][0]["addedTags"][0]) == "[literal]");
    result =
      fixture.run({"track", "update", "1", "--title", "Must not commit", "--credit=true", "performer", "Name", ""});
    CHECK(result.status == static_cast<int>(CLI::ExitCodes::ArgumentMismatch));
    CHECK(result.out.empty());
    result = fixture.run({"-O", "json", "track", "show", "1"});
    REQUIRE(result.status == 0);
    auto afterRejectedCredit = parseYaml(result.out);
    CHECK(yaml::scalarView(afterRejectedCredit.rootref()["title"]) == "Renamed");
    CHECK(yaml::scalarView(afterRejectedCredit.rootref()["custom"]["source"]) == "[manual]");

    result = fixture.run({"-O", "yaml", "track", "delete", "1"});
    REQUIRE(result.status == 0);
    auto deleteTree = parseYaml(result.out);
    CHECK(yaml::scalarView(deleteTree.rootref()["action"]) == "delete");
    CHECK(yaml::scalarView(deleteTree.rootref()["trackId"]) == "1");

    // lib export writes its destination through --output-file; the format
    // option and JSON shape depth live in LibCommandTest.
    result = fixture.run({"lib", "export", "--output-file", (fixture.root() / "library.yaml").string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(std::filesystem::exists(fixture.root() / "library.yaml"));
  }

  TEST_CASE("CLI - subtree deletion reports tag impact without internal optional names", "[cli][integration][output]")
  {
    auto const* const format = GENERATE("yaml", "json");
    auto fixture = CliFixture{};
    auto const trackId = fixture.addTrack(library::test::TrackSpec{.title = "Tagged", .tags = {"subtree"}});
    auto const createList = [&fixture](std::string_view name, std::string_view parent, std::string_view filter)
    {
      auto const create =
        fixture.run({"-O", "json", "list", "create", "--name", name, "--parent", parent, "--filter", filter});
      REQUIRE(create.status == 0);
      auto const tree = parseYaml(create.out);
      return std::string{yaml::scalarView(tree.rootref()["listId"])};
    };
    auto const rootId = createList("Root", "0", "#subtree");
    // The child references the same tag, so impact must stay on the root and
    // exclude the child from the surviving references.
    auto const childId = createList("Child", rootId, "#subtree");
    auto const survivorId = createList("Survivor", "0", "#subtree");
    auto const checkReport = [&](std::string_view output, std::string_view dryRun)
    {
      auto const tree = parseYaml(output);
      auto const report = tree.rootref();
      CHECK(yaml::scalarView(report["action"]) == "delete-subtree");
      CHECK(yaml::scalarView(report["dryRun"]) == dryRun);
      CHECK(yaml::scalarView(report["rootListId"]) == rootId);
      auto const deleted = report["deletedLists"];
      REQUIRE(deleted.is_seq());
      REQUIRE(deleted.num_children() == 2);
      CHECK(yaml::scalarView(deleted[0]["listId"]) == rootId);
      CHECK(yaml::scalarView(deleted[0]["name"]) == "Root");
      CHECK(yaml::scalarView(deleted[0]["forgottenPositionCount"]) == "0");
      CHECK_FALSE(deleted[0].has_child("orderTrackIdCount"));
      CHECK_FALSE(deleted[0].has_child("optTagImpact"));
      REQUIRE(deleted[0].has_child("tagImpact"));
      auto const impact = deleted[0]["tagImpact"];
      REQUIRE(impact.is_map());
      CHECK(yaml::scalarView(impact["tag"]) == "subtree");
      CHECK(yaml::scalarView(impact["taggedTrackCount"]) == "1");
      CHECK(yaml::scalarView(impact["removedFromTrackCount"]) == "0");
      auto const references = impact["otherListReferences"];
      REQUIRE(references.is_seq());
      REQUIRE(references.num_children() == 1);
      CHECK(yaml::scalarView(references[0]["listId"]) == survivorId);
      CHECK(yaml::scalarView(references[0]["name"]) == "Survivor");
      CHECK(yaml::scalarView(deleted[1]["listId"]) == childId);
      CHECK(yaml::scalarView(deleted[1]["name"]) == "Child");
      CHECK_FALSE(deleted[1].has_child("tagImpact"));
      CHECK_FALSE(deleted[1].has_child("optTagImpact"));
    };

    auto result = fixture.run({"-O", format, "list", "delete", rootId, "--descendants", "--dry-run"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    checkReport(result.out, "true");
    REQUIRE(fixture.run({"list", "show", rootId}).status == 0);
    REQUIRE(fixture.run({"list", "show", childId}).status == 0);

    result = fixture.run({"-O", format, "list", "delete", rootId, "--descendants"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    checkReport(result.out, "false");
    checkDomainFailure(fixture.run({"list", "show", rootId}), "list not found");
    checkDomainFailure(fixture.run({"list", "show", childId}), "list not found");
    REQUIRE(fixture.run({"list", "show", survivorId}).status == 0);
    result = fixture.run({"-O", format, "tag", "show", std::to_string(trackId.raw())});
    REQUIRE(result.status == 0);
    auto const tagTree = parseYaml(result.out);
    REQUIRE(tagTree.rootref()["tags"].num_children() == 1);
    CHECK(yaml::scalarView(tagTree.rootref()["tags"][0]) == "subtree");
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

    auto const trackId = fixture.addTrack(library::test::makeEmptyTrackSpec("empty-credits.flac"));
    result = fixture.run({"-O", "yaml", "track", "show", std::to_string(trackId.raw())});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    auto const track = tree.rootref()["tracks"][0];
    REQUIRE(track["credits"].is_seq());
    CHECK(track["credits"].num_children() == 0);
    CHECK_FALSE(track.has_child("conductor"));
    CHECK_FALSE(track.has_child("ensemble"));
    CHECK_FALSE(track.has_child("soloist"));
    CHECK_FALSE(track.has_child("musicians"));
  }
} // namespace ao::cli::test
