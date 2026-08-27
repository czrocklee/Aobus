// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/list/ListTreeProjection.h>

#include "test/unit/MessageCatalogTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/VirtualListIds.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("buildListTreeProjection projects nested list rows", "[uimodel][unit][library][list]")
  {
    auto const parentId = ListId{2};
    auto const childId = ListId{3};
    auto const projection = buildListTreeProjection(
      ao::test::englishMessageCatalog(),
      std::vector{rt::ListNode{.id = childId, .parentId = parentId, .name = "Smart Child", .expression = "genre:rock"},
                  rt::ListNode{.id = parentId, .parentId = kInvalidListId, .name = "Parent"}});

    CHECK(projection.rootIds == std::vector{rt::kAllTracksListId, parentId});

    auto const& allTracks = projection.rowsById.at(rt::kAllTracksListId);
    CHECK(allTracks.name == "All Tracks");
    CHECK(allTracks.childIds.empty());

    auto const& parent = projection.rowsById.at(parentId);
    CHECK(parent.parentId == kInvalidListId);
    CHECK(parent.name == "Parent");
    CHECK(parent.childIds == std::vector{childId});

    auto const& child = projection.rowsById.at(childId);
    CHECK(child.parentId == parentId);
    CHECK(child.name == "Smart Child");
    CHECK(child.localExpression == "genre:rock");
    CHECK(child.childIds.empty());
  }

  TEST_CASE("buildListTreeProjection localizes the virtual library row", "[uimodel][unit][list][localization]")
  {
    auto const catalog = ao::test::messageCatalog("de-AT");
    auto const projection = buildListTreeProjection(catalog, {});

    REQUIRE(projection.rowsById.contains(rt::kAllTracksListId));
    CHECK(projection.rowsById.at(rt::kAllTracksListId).name == "Alle Titel");
  }

  TEST_CASE("buildListTreeProjection places invalid parents beside All Tracks", "[uimodel][unit][library][list]")
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

  TEST_CASE("buildListTreeProjection breaks parent cycles at the lowest cycle id", "[uimodel][unit][library][list]")
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

  TEST_CASE("buildListTreeProjection orders children by list id", "[uimodel][unit][library][list]")
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
