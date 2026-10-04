// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ryml.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::cli::test
{
  namespace
  {
    std::string_view requireField(ryml::ConstNodeRef node, std::string_view key)
    {
      auto const value = yaml::scalarView(yaml::findChild(node, key));
      REQUIRE_FALSE(value.empty());
      return value;
    }

    std::vector<std::string> storedOrderIds(std::string_view dumpOutput, std::uint32_t listId)
    {
      auto tree = parseYaml(dumpOutput);
      auto const lists = yaml::findChild(tree.rootref(), "lists");
      REQUIRE(lists.is_seq());

      auto const id = std::to_string(listId);
      auto optTrackIds = std::optional<std::vector<std::string>>{};

      for (auto const listNode : lists.children())
      {
        if (yaml::scalarView(yaml::findChild(listNode, "id")) == id)
        {
          auto const order = yaml::findChild(listNode, "order");
          REQUIRE(order.is_seq());

          auto& trackIds = optTrackIds.emplace();
          trackIds.reserve(order.num_children());

          for (auto const trackId : order.children())
          {
            trackIds.emplace_back(yaml::scalarView(trackId));
          }

          break;
        }
      }

      // REQUIRE rather than FAIL: MSVC treats a return after FAIL as unreachable (C4702).
      INFO("list dump contains no list with id " << id);
      REQUIRE(optTrackIds);
      return std::move(*optTrackIds);
    }

    struct SubtreeEntry final
    {
      std::string listId{};
      std::string name{};
      std::string forgottenPositionCount{};

      bool operator==(SubtreeEntry const&) const = default;
    };

    struct SubtreeReport final
    {
      std::string action{};
      std::string dryRun{};
      std::string rootListId{};
      std::vector<SubtreeEntry> entries{};
    };

    SubtreeReport parseSubtreeReport(std::string_view output)
    {
      auto tree = parseYaml(output);
      auto const root = tree.rootref();
      auto report = SubtreeReport{};
      report.action = std::string{requireField(root, "action")};
      report.dryRun = std::string{requireField(root, "dryRun")};
      report.rootListId = std::string{requireField(root, "rootListId")};

      auto const deletedLists = yaml::findChild(root, "deletedLists");
      REQUIRE(deletedLists.is_seq());
      report.entries.reserve(deletedLists.num_children());

      for (auto const entry : deletedLists.children())
      {
        report.entries.push_back(SubtreeEntry{
          .listId = std::string{requireField(entry, "listId")},
          .name = std::string{requireField(entry, "name")},
          .forgottenPositionCount = std::string{requireField(entry, "forgottenPositionCount")},
        });
      }

      return report;
    }

    // CLI11 rejects the unknown flag as a usage failure, distinct from domain exit 1.
    void checkDryRunRejected(CliResult const& result)
    {
      CHECK(result.status != 0);
      CHECK(result.status != 1);
      CHECK(result.out.empty());
      CHECK(contains(result.err, "--dry-run"));
    }

    void requireOrderReport(std::string_view output,
                            std::string_view expectedAction,
                            std::string_view expectedListId,
                            std::string_view expectedStatus,
                            std::string_view expectedForgottenPositionCount)
    {
      auto tree = parseYaml(output);
      auto const root = tree.rootref();
      REQUIRE(root.is_map());
      CHECK(requireField(root, "action") == expectedAction);
      CHECK(requireField(root, "listId") == expectedListId);
      CHECK(requireField(root, "status") == expectedStatus);

      auto const selectedTrackIds = yaml::findChild(root, "selectedTrackIds");
      REQUIRE(selectedTrackIds.is_seq());
      CHECK(selectedTrackIds.num_children() == 0);

      CHECK_FALSE(root.has_child("beforeTrackId"));
      CHECK_FALSE(root.has_child("optForgottenPositionCount"));
      CHECK(requireField(root, "forgottenPositionCount") == expectedForgottenPositionCount);
    }
  } // namespace

  TEST_CASE("CLI - list delete --descendants previews and commits the subtree in root-first order",
            "[cli][integration][list][list-delete]")
  {
    auto fixture = CliFixture{};
    auto const first = fixture.addTrack(library::test::TrackSpec{.title = "Alpha", .uri = "alpha.flac"});
    fixture.addTrack(library::test::TrackSpec{.title = "Beta", .uri = "beta.flac"});
    auto const third = fixture.addTrack(library::test::TrackSpec{.title = "Gamma", .uri = "gamma.flac"});

    auto result = fixture.run({"list", "create", "--name", "Root"});
    REQUIRE(result.status == 0);
    auto const rootId = parseCreatedListId(result.out);

    result = fixture.run({"list", "create", "--name", "Child", "--parent", std::to_string(rootId)});
    REQUIRE(result.status == 0);
    auto const childId = parseCreatedListId(result.out);

    result = fixture.run({"list", "create", "--name", "Grandchild", "--parent", std::to_string(childId)});
    REQUIRE(result.status == 0);
    auto const grandchildId = parseCreatedListId(result.out);

    result = fixture.run({"list", "create", "--name", "Keeper"});
    REQUIRE(result.status == 0);
    auto const keeperId = parseCreatedListId(result.out);

    // A real move that changes the effective order stores one saved position
    // per member, so the root reports a nonzero forgotten count.
    result = fixture.run({"list",
                          "order",
                          "move",
                          std::to_string(rootId),
                          std::to_string(third.raw()),
                          "--before",
                          std::to_string(first.raw())});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "json", "list", "dump"});
    REQUIRE(result.status == 0);
    REQUIRE(storedOrderIds(result.out, rootId).size() == 3);

    result = fixture.run({"list", "delete", "--descendants", "--dry-run", std::to_string(rootId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "would delete list subtree: " + std::to_string(rootId) + " (3 Lists)"));
    CHECK(contains(result.out, "Root (forget 3 saved positions)"));
    CHECK(contains(result.out, "Grandchild (forget 0 saved positions)"));

    result = fixture.run({"-O", "yaml", "list", "delete", "--descendants", "--dry-run", std::to_string(rootId)});
    REQUIRE(result.status == 0);
    auto const dryRun = parseSubtreeReport(result.out);
    CHECK(dryRun.action == "delete-subtree");
    CHECK(dryRun.dryRun == "true");
    CHECK(dryRun.rootListId == std::to_string(rootId));
    REQUIRE(dryRun.entries.size() == 3);
    CHECK(dryRun.entries[0].listId == std::to_string(rootId));
    CHECK(dryRun.entries[0].name == "Root");
    CHECK(dryRun.entries[0].forgottenPositionCount == "3");
    CHECK(dryRun.entries[1].listId == std::to_string(childId));
    CHECK(dryRun.entries[1].name == "Child");
    CHECK(dryRun.entries[1].forgottenPositionCount == "0");
    CHECK(dryRun.entries[2].listId == std::to_string(grandchildId));
    CHECK(dryRun.entries[2].name == "Grandchild");
    CHECK(dryRun.entries[2].forgottenPositionCount == "0");

    // The preview commits nothing: every list and the root's saved order remain.
    result = fixture.run({"list", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Root"));
    CHECK(contains(result.out, "Child"));
    CHECK(contains(result.out, "Grandchild"));
    CHECK(contains(result.out, "Keeper"));

    result = fixture.run({"-O", "json", "list", "dump"});
    REQUIRE(result.status == 0);
    REQUIRE(storedOrderIds(result.out, rootId).size() == 3);

    result = fixture.run({"-O", "json", "list", "delete", "--descendants", std::to_string(rootId)});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    auto const committed = parseSubtreeReport(result.out);
    CHECK(committed.action == "delete-subtree");
    CHECK(committed.dryRun == "false");
    CHECK(committed.rootListId == std::to_string(rootId));
    CHECK(committed.entries == dryRun.entries);

    // The whole chain is gone root-first and the unrelated list survives.
    result = fixture.run({"list", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Keeper"));
    CHECK_FALSE(contains(result.out, "Root"));
    CHECK_FALSE(contains(result.out, "Child"));
    CHECK_FALSE(contains(result.out, "Grandchild"));

    result = fixture.run({"-O", "json", "list", "dump"});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    auto const lists = yaml::findChild(tree.rootref(), "lists");
    REQUIRE(lists.is_seq());
    REQUIRE(lists.num_children() == 1);
    CHECK(yaml::scalarView(yaml::findChild(lists[0], "id")) == std::to_string(keeperId));
    CHECK(yaml::scalarView(yaml::findChild(lists[0], "name")) == "Keeper");

    checkDomainFailure(fixture.run({"list", "delete", "--descendants", "--dry-run", "999"}), "list not found: 999");
  }

  TEST_CASE("CLI - ordinary list delete refuses a parent until its children are removed",
            "[cli][integration][list][list-delete]")
  {
    auto fixture = CliFixture{};

    auto result = fixture.run({"list", "create", "--name", "Parent"});
    REQUIRE(result.status == 0);
    auto const parentId = parseCreatedListId(result.out);

    result = fixture.run({"list", "create", "--name", "Child", "--parent", std::to_string(parentId)});
    REQUIRE(result.status == 0);
    auto const childId = parseCreatedListId(result.out);

    checkDomainFailure(fixture.run({"list", "delete", std::to_string(parentId)}), "has dependent Lists: Child");
    checkDomainFailure(
      fixture.run({"list", "delete", "--dry-run", std::to_string(parentId)}), "has dependent Lists: Child");

    // Both refusals leave the hierarchy untouched.
    result = fixture.run({"list", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Parent"));
    CHECK(contains(result.out, "Child"));

    result = fixture.run({"list", "delete", std::to_string(childId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "deleted list: " + std::to_string(childId)));

    // Once childless, the parent deletes through the subtree path too, and the
    // singular list count reads "1 List".
    result = fixture.run({"list", "delete", "--descendants", std::to_string(parentId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "deleted list subtree: " + std::to_string(parentId) + " (1 List)"));

    result = fixture.run({"list", "show"});
    REQUIRE(result.status == 0);
    CHECK_FALSE(contains(result.out, "Parent"));
    CHECK_FALSE(contains(result.out, "Child"));
  }

  TEST_CASE("CLI - list order reset forgets every saved position of that list only",
            "[cli][integration][list][list-order]")
  {
    auto fixture = CliFixture{};
    auto const first =
      fixture.addTrack(library::test::TrackSpec{.title = "First", .uri = "first.flac", .tags = {"playlist"}});
    fixture.addTrack(library::test::TrackSpec{.title = "Second", .uri = "second.flac", .tags = {"playlist"}});
    auto const third =
      fixture.addTrack(library::test::TrackSpec{.title = "Third", .uri = "third.flac", .tags = {"playlist"}});
    auto const fourth =
      fixture.addTrack(library::test::TrackSpec{.title = "Fourth", .uri = "fourth.flac", .tags = {"other"}});
    auto const fifth =
      fixture.addTrack(library::test::TrackSpec{.title = "Fifth", .uri = "fifth.flac", .tags = {"other"}});

    auto result = fixture.run({"list", "create", "--name", "Playlist", "--filter", "#playlist"});
    REQUIRE(result.status == 0);
    auto const playlistId = parseCreatedListId(result.out);

    result = fixture.run({"list", "create", "--name", "Other", "--filter", "#other"});
    REQUIRE(result.status == 0);
    auto const otherId = parseCreatedListId(result.out);

    result = fixture.run({"list",
                          "order",
                          "move",
                          std::to_string(playlistId),
                          std::to_string(third.raw()),
                          "--before",
                          std::to_string(first.raw())});
    REQUIRE(result.status == 0);

    result = fixture.run({"list",
                          "order",
                          "move",
                          std::to_string(otherId),
                          std::to_string(fifth.raw()),
                          "--before",
                          std::to_string(fourth.raw())});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "json", "list", "dump"});
    REQUIRE(result.status == 0);
    auto const playlistOrder = storedOrderIds(result.out, playlistId);
    REQUIRE(playlistOrder.size() == 3);
    auto const otherOrder = storedOrderIds(result.out, otherId);
    REQUIRE(otherOrder.size() == 2);

    // Generic tag removal hides a saved position without discarding it. Reset
    // must clear that position too, unlike forget-hidden's visible-rank policy.
    result = fixture.run({"tag", "remove", "playlist", std::to_string(third.raw())});
    REQUIRE(result.status == 0);
    result = fixture.run({"list", "show", std::to_string(playlistId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Tracks: 2"));
    CHECK_FALSE(contains(result.out, "Third"));

    // Parse and lookup failures are observed while both lists still have ranks.
    checkDryRunRejected(fixture.run({"list", "order", "reset", std::to_string(playlistId), "--dry-run"}));
    checkDomainFailure(fixture.run({"list", "order", "reset", "999"}), "List 999 does not exist");
    result = fixture.run({"-O", "json", "list", "dump"});
    REQUIRE(result.status == 0);
    CHECK(storedOrderIds(result.out, playlistId) == playlistOrder);
    CHECK(storedOrderIds(result.out, otherId) == otherOrder);

    result = fixture.run({"-O", "json", "list", "order", "reset", std::to_string(playlistId)});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    requireOrderReport(
      result.out, "reset", std::to_string(playlistId), "applied", std::to_string(playlistOrder.size()));

    // Reset clears only its own list; the sibling keeps its saved ranks.
    result = fixture.run({"-O", "json", "list", "dump"});
    REQUIRE(result.status == 0);
    CHECK(storedOrderIds(result.out, playlistId).empty());
    CHECK(storedOrderIds(result.out, otherId) == otherOrder);

    result = fixture.run({"list", "order", "reset", std::to_string(otherId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out,
                   "reset list order: " + std::to_string(otherId) + " (applied; forgot " +
                     std::to_string(otherOrder.size()) + " positions)"));

    // An empty order makes reset a no-op that reports zero forgotten positions.
    result = fixture.run({"list", "order", "reset", std::to_string(playlistId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "reset list order: " + std::to_string(playlistId) + " (no-op; forgot 0 positions)"));

    result = fixture.run({"-O", "yaml", "list", "order", "reset", std::to_string(playlistId)});
    REQUIRE(result.status == 0);
    requireOrderReport(result.out, "reset", std::to_string(playlistId), "no-op", "0");

    result = fixture.run({"-O", "json", "list", "dump"});
    REQUIRE(result.status == 0);
    CHECK(storedOrderIds(result.out, playlistId).empty());
    CHECK(storedOrderIds(result.out, otherId).empty());
  }

  TEST_CASE("CLI - list order forget-hidden removes only hidden positions and preserves visible ranks",
            "[cli][integration][list][list-order]")
  {
    auto fixture = CliFixture{};
    auto const alpha =
      fixture.addTrack(library::test::TrackSpec{.title = "Alpha", .uri = "alpha.flac", .tags = {"playlist"}});
    fixture.addTrack(library::test::TrackSpec{.title = "Beta", .uri = "beta.flac", .tags = {"playlist"}});
    auto const gamma =
      fixture.addTrack(library::test::TrackSpec{.title = "Gamma", .uri = "gamma.flac", .tags = {"playlist"}});

    auto result = fixture.run({"list", "create", "--name", "Playlist", "--filter", "#playlist"});
    REQUIRE(result.status == 0);
    auto const playlistId = parseCreatedListId(result.out);

    result = fixture.run({"list",
                          "order",
                          "move",
                          std::to_string(playlistId),
                          std::to_string(gamma.raw()),
                          "--before",
                          std::to_string(alpha.raw())});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "json", "list", "dump"});
    REQUIRE(result.status == 0);
    auto const orderBefore = storedOrderIds(result.out, playlistId);
    REQUIRE(orderBefore.size() == 3);
    CHECK(orderBefore.front() == std::to_string(gamma.raw()));

    // The generic tag command removes membership without touching saved
    // ranks, which leaves gamma's position hidden.
    result = fixture.run({"tag", "remove", "playlist", std::to_string(gamma.raw())});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "removed tag: playlist from 1 track(s)"));

    result = fixture.run({"list", "show", std::to_string(playlistId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Tracks: 2"));
    CHECK(contains(result.out, "Alpha"));
    CHECK_FALSE(contains(result.out, "Gamma"));

    // A rejected preview flag must not fall through to the real cleanup.
    checkDryRunRejected(fixture.run({"list", "order", "forget-hidden", std::to_string(playlistId), "--dry-run"}));
    result = fixture.run({"-O", "json", "list", "dump"});
    REQUIRE(result.status == 0);
    CHECK(storedOrderIds(result.out, playlistId) == orderBefore);

    result = fixture.run({"-O", "yaml", "list", "order", "forget-hidden", std::to_string(playlistId)});
    REQUIRE(result.status == 0);
    requireOrderReport(result.out, "forget-hidden", std::to_string(playlistId), "applied", "1");

    // Only the hidden position disappeared; the visible tracks keep their
    // relative ranks from the saved order.
    result = fixture.run({"-O", "json", "list", "dump"});
    REQUIRE(result.status == 0);
    auto const orderAfter = storedOrderIds(result.out, playlistId);
    auto const expectedVisibleOrder = std::vector<std::string>(orderBefore.begin() + 1, orderBefore.end());
    REQUIRE(orderAfter.size() == 2);
    CHECK(orderAfter == expectedVisibleOrder);

    result = fixture.run({"list", "order", "forget-hidden", std::to_string(playlistId)});
    REQUIRE(result.status == 0);
    CHECK(
      contains(result.out, "forget-hidden list order: " + std::to_string(playlistId) + " (no-op; forgot 0 positions)"));

    result = fixture.run({"-O", "json", "list", "order", "forget-hidden", std::to_string(playlistId)});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    requireOrderReport(result.out, "forget-hidden", std::to_string(playlistId), "no-op", "0");

    result = fixture.run({"-O", "json", "list", "dump"});
    REQUIRE(result.status == 0);
    CHECK(storedOrderIds(result.out, playlistId) == orderAfter);

    checkDomainFailure(fixture.run({"list", "order", "forget-hidden", "999"}), "List 999 does not exist");
  }
} // namespace ao::cli::test
