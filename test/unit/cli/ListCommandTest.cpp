// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ListCommand.h"

#include "CliTestSupport.h"
#include <ao/Error.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace ao::cli::test
{
  namespace
  {
    std::uint32_t parseCreatedListId(std::string_view output)
    {
      auto constexpr kPrefix = std::string_view{"add list: "};
      auto const start = output.find(kPrefix);
      REQUIRE(start != std::string_view::npos);

      auto const idStart = start + kPrefix.size();
      auto const idEnd = output.find(' ', idStart);
      REQUIRE(idEnd != std::string_view::npos);

      return static_cast<std::uint32_t>(std::stoul(std::string{output.substr(idStart, idEnd - idStart)}));
    }

    std::uint32_t parseFirstTrackId(std::string_view output)
    {
      auto const idStart = output.find_first_of("0123456789");
      REQUIRE(idStart != std::string_view::npos);
      auto const idEnd = output.find(' ', idStart);
      REQUIRE(idEnd != std::string_view::npos);
      return static_cast<std::uint32_t>(std::stoul(std::string{output.substr(idStart, idEnd - idStart)}));
    }

    std::uint32_t parseJsonUintField(std::string_view output, std::string_view field)
    {
      auto tree = parseYaml(output);
      auto const value = yaml::scalarView(yaml::findChild(tree.rootref(), field));
      REQUIRE_FALSE(value.empty());
      return static_cast<std::uint32_t>(std::stoul(std::string{value}));
    }
  } // namespace

  TEST_CASE("CLI - List order authoring statuses preserve command failure semantics", "[cli][unit][list][list-order]")
  {
    CHECK(validateListOrderCommandStatus(rt::AuthoringStatus::Applied));
    CHECK(validateListOrderCommandStatus(rt::AuthoringStatus::NoOp));

    auto const staleRes = validateListOrderCommandStatus(rt::AuthoringStatus::Stale);
    REQUIRE_FALSE(staleRes);
    CHECK(staleRes.error().code == Error::Code::Conflict);
    CHECK(staleRes.error().message == "List order target became stale");

    auto const unavailableRes = validateListOrderCommandStatus(rt::AuthoringStatus::Unavailable);
    REQUIRE_FALSE(unavailableRes);
    CHECK(unavailableRes.error().code == Error::Code::InvalidState);
    CHECK(unavailableRes.error().message == "Library is busy");
  }

  TEST_CASE("CLI - list create and delete round-trip through the library", "[cli][integration][list]")
  {
    auto fixture = CliFixture{};

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"list", "create", "--name", "My List"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "add list:"));
    CHECK(contains(result.out, "My List"));
    auto const listId = parseCreatedListId(result.out);

    result = fixture.run({"list", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "My List"));

    result = fixture.run({"list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "List ID:"));
    CHECK(contains(result.out, "My List"));
    CHECK(contains(result.out, "Tracks: 0"));

    result = fixture.run({"-O", "json", "list", "show"});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    auto tree = parseYaml(result.out);
    bool foundList = false;

    for (auto const listNode : tree.rootref()["lists"].children())
    {
      foundList = foundList || yaml::scalarView(listNode["name"]) == "My List";
    }

    CHECK(foundList);

    result = fixture.run({"list", "delete", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "deleted list:"));

    result = fixture.run({"list", "show"});
    REQUIRE(result.status == 0);
    CHECK_FALSE(contains(result.out, "My List"));
  }

  TEST_CASE("CLI - writable-tag membership and saved order round-trip", "[cli][integration][list]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");
    fixture.copyAudio("hires.flac", "hires.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"list", "create", "--name", "Playlist", "--filter", "#playlist"});
    REQUIRE(result.status == 0);
    auto const listId = parseCreatedListId(result.out);

    result = fixture.run({"list", "add", std::to_string(listId), "2"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Added #playlist to 1 track"));

    result = fixture.run({"-O", "json", "list", "add", std::to_string(listId), "1", "2", "1"});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["tag"]) == "playlist");
    CHECK(yaml::scalarView(tree.rootref()["changed"]) == "true");
    REQUIRE(tree.rootref()["targetTrackIds"].num_children() == 2);
    CHECK(yaml::scalarView(tree.rootref()["targetTrackIds"][0]) == "1");
    CHECK(yaml::scalarView(tree.rootref()["targetTrackIds"][1]) == "2");
    REQUIRE(tree.rootref()["changes"].num_children() == 1);
    CHECK(yaml::scalarView(tree.rootref()["changes"][0]["trackId"]) == "1");

    result = fixture.run({"list", "order", "move", std::to_string(listId), "2", "--before", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "moved tracks in list:"));

    result = fixture.run({"-O", "json", "list", "dump"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    REQUIRE(tree.rootref()["lists"].is_seq());
    REQUIRE(tree.rootref()["lists"].num_children() == 1);
    auto const order = tree.rootref()["lists"][0]["order"];
    REQUIRE(order.is_seq());
    REQUIRE(order.num_children() == 2);
    CHECK(yaml::scalarView(order[0]) == "2");
    CHECK(yaml::scalarView(order[1]) == "1");

    result = fixture.run({"list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Tracks: 2"));
    CHECK(contains(result.out, "Test Title"));
    CHECK(contains(result.out, "HiRes Title"));

    result = fixture.run({"list", "remove", std::to_string(listId), "2"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Removed #playlist from 1 track"));
    CHECK(contains(result.out, "forgot 1 saved position"));

    result = fixture.run({"-O", "json", "list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    tree = parseYaml(result.out);
    REQUIRE(tree.rootref()["tracks"].is_seq());
    CHECK(tree.rootref()["tracks"].num_children() == 1);

    result = fixture.run({"list", "update", std::to_string(listId), "--name", "Pinned", "--desc", "Pinned songs"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "updated list:"));

    result = fixture.run({"list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Pinned"));
    CHECK(contains(result.out, "Pinned songs"));
  }

  TEST_CASE("CLI - list show resolves filtered List tracks", "[cli][integration][list]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");
    fixture.copyAudio("hires.flac", "hires.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"list", "create", "--name", "Smart", "--filter", "$title ~ \"Test\""});
    REQUIRE(result.status == 0);
    auto const listId = parseCreatedListId(result.out);

    result = fixture.run({"list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Filter: $title ~ \"Test\""));
    CHECK(contains(result.out, "Tracks: 1"));
    CHECK(contains(result.out, "Test Title"));
    CHECK_FALSE(contains(result.out, "HiRes Title"));

    result = fixture.run({"list", "update", std::to_string(listId), "--filter", "$title ~ \"Title\""});
    REQUIRE(result.status == 0);

    result = fixture.run({"list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Tracks: 2"));
    CHECK(contains(result.out, "Test Title"));
    CHECK(contains(result.out, "HiRes Title"));

    checkDomainFailure(fixture.run({"list", "add", std::to_string(listId), "1"}), "membership is computed");
  }

  TEST_CASE("CLI - child List detail honors parent membership", "[cli][integration][list]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");
    fixture.copyAudio("hires.flac", "hires.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"track", "show", "--filter", "$title ~ \"Test\""});
    REQUIRE(result.status == 0);
    auto const testTrackId = parseFirstTrackId(result.out);

    result = fixture.run({"list", "create", "--name", "Parent", "--filter", "#parent"});
    REQUIRE(result.status == 0);
    auto const parentId = parseCreatedListId(result.out);

    result = fixture.run({"list", "add", std::to_string(parentId), std::to_string(testTrackId)});
    REQUIRE(result.status == 0);

    result = fixture.run(
      {"list", "create", "--name", "Child", "--parent", std::to_string(parentId), "--filter", "$title ~ \"Title\""});
    REQUIRE(result.status == 0);
    auto const smartId = parseCreatedListId(result.out);

    result = fixture.run({"list", "show", std::to_string(smartId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Tracks: 1"));
    CHECK(contains(result.out, "Test Title"));
    CHECK_FALSE(contains(result.out, "HiRes Title"));
  }

  TEST_CASE("CLI - list mutations support dry-run reports", "[cli][integration][list][dry-run]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "json", "list", "create", "--dry-run", "--name", "Playlist", "--filter", "#pinned"});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK_FALSE(tree.rootref()["listId"].readable());
    CHECK(yaml::scalarView(tree.rootref()["name"]) == "Playlist");

    result = fixture.run({"list", "show"});
    REQUIRE(result.status == 0);
    CHECK_FALSE(contains(result.out, "Playlist"));

    result = fixture.run({"-O", "json", "list", "create", "--name", "Playlist", "--filter", "#pinned"});
    REQUIRE(result.status == 0);
    auto const listId = parseJsonUintField(result.out, "listId");
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");

    result = fixture.run({"-O", "json", "list", "update", "--dry-run", std::to_string(listId), "--name", "Pinned"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["fields"][0]["field"]) == "name");
    CHECK(yaml::scalarView(tree.rootref()["fields"][0]["newValue"]) == "Pinned");

    result = fixture.run({"list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Playlist"));
    CHECK_FALSE(contains(result.out, "Pinned"));

    result = fixture.run({"list", "update", std::to_string(listId), "--name", "Pinned"});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "json", "list", "add", "--dry-run", std::to_string(listId), "1", "1"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["action"]) == "add");
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["changed"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["tag"]) == "pinned");
    REQUIRE(tree.rootref()["targetTrackIds"].num_children() == 1);
    CHECK(yaml::scalarView(tree.rootref()["targetTrackIds"][0]) == "1");
    REQUIRE(tree.rootref()["changes"].num_children() == 1);
    CHECK(yaml::scalarView(tree.rootref()["changes"][0]["trackId"]) == "1");

    result = fixture.run({"list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Tracks: 0"));

    checkDomainFailure(
      fixture.run({"list", "add", "--dry-run", std::to_string(listId), "1", "9999"}), "track not found: 9999");

    result = fixture.run({"list", "add", std::to_string(listId), "1", "1"});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "json", "list", "remove", "--dry-run", std::to_string(listId), "1", "1"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["action"]) == "remove");
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["changed"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["tag"]) == "pinned");
    REQUIRE(tree.rootref()["targetTrackIds"].num_children() == 1);
    CHECK(yaml::scalarView(tree.rootref()["targetTrackIds"][0]) == "1");
    REQUIRE(tree.rootref()["changes"].num_children() == 1);
    CHECK(yaml::scalarView(tree.rootref()["changes"][0]["trackId"]) == "1");
    CHECK(tree.rootref()["forgottenPositionTrackIds"].num_children() == 0);

    result = fixture.run({"list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Tracks: 1"));

    result = fixture.run({"list", "remove", std::to_string(listId), "1", "1"});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "json", "list", "delete", "--dry-run", std::to_string(listId)});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["name"]) == "Pinned");

    result = fixture.run({"list", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Pinned"));

    result = fixture.run({"-O", "json", "list", "delete", std::to_string(listId)});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");

    result = fixture.run({"list", "show"});
    REQUIRE(result.status == 0);
    CHECK_FALSE(contains(result.out, "Pinned"));
  }

  TEST_CASE("CLI - list domain failures use stderr and exit non-zero", "[cli][unit][list][contract]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    checkDomainFailure(fixture.run({"list", "delete", "999"}), "list not found: 999");
    checkDomainFailure(fixture.run({"list", "update", "999", "--name", "Missing"}), "list not found: 999");
    checkDomainFailure(fixture.run({"list", "create", "--name", "Bad", "--filter", "("}), "invalid list filter");
    checkDomainFailure(
      fixture.run({"list", "create", "--name", "Bad Parent", "--parent", "999"}), "list parent not found");

    result = fixture.run({"list", "create", "--name", "Parent"});
    REQUIRE(result.status == 0);
    auto const parentId = parseCreatedListId(result.out);
    checkDomainFailure(fixture.run({"list", "update", std::to_string(parentId), "--parent", std::to_string(parentId)}),
                       "list parent cannot be the list itself");
    checkDomainFailure(
      fixture.run({"-O", "json", "list", "dump", "--raw"}), "list dump --raw supports only plain output");
  }
} // namespace ao::cli::test
