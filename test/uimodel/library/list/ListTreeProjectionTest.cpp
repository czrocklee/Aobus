// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/list/ListTreeProjection.h>

#include "test/MessageCatalogTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/VirtualListIds.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("listTreeAncestorIds returns distinct parents nearest first")
  {
    auto const start = ListId{2};
    auto const parent = ListId{3};
    auto const grandparent = ListId{4};
    auto const missing = ListId{99};
    auto projection = ListTreeProjection{};
    projection.rowsById.emplace(start, ListTreeProjectionRow{.id = start, .parentId = parent, .name = "Child"});
    projection.rowsById.emplace(parent, ListTreeProjectionRow{.id = parent, .parentId = grandparent, .name = "Parent"});
    projection.rowsById.emplace(grandparent, ListTreeProjectionRow{.id = grandparent, .name = "Root"});

    SECTION("a complete chain ends at the root")
    {
      CHECK(listTreeAncestorIds(projection, start) == std::vector{parent, grandparent});
      CHECK(listTreeAncestorIds(projection, grandparent).empty());
      CHECK(listTreeAncestorIds(projection, missing).empty());
    }

    SECTION("a cycle through the starting row never includes it")
    {
      projection.rowsById.at(grandparent).parentId = start;
      CHECK(listTreeAncestorIds(projection, start) == std::vector{parent, grandparent});
    }

    SECTION("a cycle above the starting row stops before repeating a parent")
    {
      projection.rowsById.at(grandparent).parentId = parent;
      CHECK(listTreeAncestorIds(projection, start) == std::vector{parent, grandparent});
    }

    SECTION("a self-parent has no ancestors")
    {
      projection.rowsById.at(start).parentId = start;
      CHECK(listTreeAncestorIds(projection, start).empty());
    }

    SECTION("the final parent need not have a row")
    {
      projection.rowsById.erase(grandparent);
      CHECK(listTreeAncestorIds(projection, start) == std::vector{parent, grandparent});
    }
  }

  TEST_CASE("buildListTreeProjection projects nested list rows")
  {
    auto const parentId = ListId{2};
    auto const childId = ListId{3};
    auto const projection = buildListTreeProjection(
      ao::test::englishMessageCatalog(),
      std::vector{rt::ListNode{.id = childId, .parentId = parentId, .name = "Smart Child", .expression = "genre:rock"},
                  rt::ListNode{.id = parentId, .parentId = kInvalidListId, .name = "Parent"}});

    CHECK(projection.rootIds == std::vector{rt::kAllTracksListId, parentId});

    REQUIRE(projection.rowsById.size() == 3);
    auto const& allTracks = projection.rowsById.at(rt::kAllTracksListId);
    CHECK(allTracks.id == rt::kAllTracksListId);
    CHECK(allTracks.isSystem);
    CHECK(allTracks.parentId == kInvalidListId);
    CHECK(allTracks.localExpression.empty());
    CHECK(allTracks.name == "All Tracks");
    CHECK(allTracks.childIds.empty());

    auto const& parent = projection.rowsById.at(parentId);
    CHECK(parent.id == parentId);
    CHECK_FALSE(parent.isSystem);
    CHECK(parent.localExpression.empty());
    CHECK(parent.parentId == kInvalidListId);
    CHECK(parent.name == "Parent");
    CHECK(parent.childIds == std::vector{childId});

    auto const& child = projection.rowsById.at(childId);
    CHECK(child.id == childId);
    CHECK_FALSE(child.isSystem);
    CHECK(child.parentId == parentId);
    CHECK(child.name == "Smart Child");
    CHECK(child.localExpression == "genre:rock");
    CHECK(child.childIds.empty());
  }

  TEST_CASE("buildListTreeProjection localizes the virtual library row")
  {
    auto const catalog = ao::test::messageCatalog("de-AT");
    auto const projection = buildListTreeProjection(catalog, {});

    REQUIRE(projection.rowsById.contains(rt::kAllTracksListId));
    CHECK(projection.rowsById.at(rt::kAllTracksListId).name == "Alle Titel");
  }

  TEST_CASE("buildListTreeProjection places invalid parents beside All Tracks")
  {
    auto const orphanId = ListId{4};
    auto const selfParentId = ListId{5};
    auto const projection = buildListTreeProjection(
      ao::test::englishMessageCatalog(),
      std::vector{rt::ListNode{.id = orphanId, .parentId = ListId{999}, .name = "Orphan"},
                  rt::ListNode{.id = selfParentId, .parentId = selfParentId, .name = "Self Parent"}});

    auto const& allTracks = projection.rowsById.at(rt::kAllTracksListId);
    CHECK(allTracks.childIds.empty());
    CHECK(projection.rootIds == std::vector{rt::kAllTracksListId, orphanId, selfParentId});
    CHECK(projection.rowsById.at(orphanId).parentId == kInvalidListId);
    CHECK(projection.rowsById.at(selfParentId).parentId == kInvalidListId);
  }

  TEST_CASE("buildListTreeProjection breaks parent cycles at the lowest cycle id")
  {
    auto const descendantId = ListId{2};
    auto const lowerCycleId = ListId{4};
    auto const higherCycleId = ListId{7};
    auto const projection =
      buildListTreeProjection(ao::test::englishMessageCatalog(),
                              std::vector{
                                rt::ListNode{.id = higherCycleId, .parentId = lowerCycleId, .name = "Higher"},
                                rt::ListNode{.id = descendantId, .parentId = higherCycleId, .name = "Descendant"},
                                rt::ListNode{.id = lowerCycleId, .parentId = higherCycleId, .name = "Lower"},
                              });

    CHECK(projection.rowsById.at(rt::kAllTracksListId).childIds.empty());
    CHECK(projection.rootIds == std::vector{rt::kAllTracksListId, lowerCycleId});
    CHECK(projection.rowsById.at(lowerCycleId).parentId == kInvalidListId);
    CHECK(projection.rowsById.at(lowerCycleId).childIds == std::vector{higherCycleId});
    CHECK(projection.rowsById.at(higherCycleId).parentId == lowerCycleId);
    CHECK(projection.rowsById.at(higherCycleId).childIds == std::vector{descendantId});
    CHECK(projection.rowsById.at(descendantId).parentId == higherCycleId);
  }

  TEST_CASE("buildListTreeProjection orders children by list id")
  {
    auto const parentId = ListId{2};
    auto const projection =
      buildListTreeProjection(ao::test::englishMessageCatalog(),
                              std::vector{
                                rt::ListNode{.id = ListId{30}, .parentId = parentId, .name = "Third in snapshot"},
                                rt::ListNode{.id = parentId, .parentId = kInvalidListId, .name = "Parent"},
                                rt::ListNode{.id = ListId{10}, .parentId = parentId, .name = "First by id"},
                                rt::ListNode{.id = ListId{20}, .parentId = parentId, .name = "Second by id"},
                              });

    CHECK(projection.rowsById.at(parentId).childIds == std::vector{ListId{10}, ListId{20}, ListId{30}});
  }
} // namespace ao::uimodel::test
