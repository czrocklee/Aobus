// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ListNode.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace ao::uimodel::test
{
  TEST_CASE("buildListTreeProjection projects nested list rows", "[uimodel][unit][library][list]")
  {
    auto const parentId = ListId{2};
    auto const childId = ListId{3};
    auto const projection = buildListTreeProjection(std::vector{
      rt::ListNode{.id = childId,
                   .parentId = parentId,
                   .name = "Smart Child",
                   .kind = rt::ListNodeKind::Smart,
                   .smartExpression = "genre:rock"},
      rt::ListNode{
        .id = parentId, .parentId = kInvalidListId, .name = "Manual Parent", .kind = rt::ListNodeKind::Manual}});

    CHECK(projection.rootIds == std::vector{rt::kAllTracksListId});

    auto const& allTracks = projection.rowsById.at(rt::kAllTracksListId);
    CHECK(allTracks.name == "All Tracks");
    CHECK(allTracks.childIds == std::vector{parentId});

    auto const& parent = projection.rowsById.at(parentId);
    CHECK(parent.name == "Manual Parent");
    CHECK(parent.childIds == std::vector{childId});

    auto const& child = projection.rowsById.at(childId);
    CHECK(child.parentId == parentId);
    CHECK(child.name == "Smart Child");
    CHECK(child.isSmart);
    CHECK(child.localExpression == "genre:rock");
    CHECK(child.childIds.empty());
  }

  TEST_CASE("buildListTreeProjection attaches invalid parents under all tracks", "[uimodel][unit][library][list]")
  {
    auto const orphanId = ListId{4};
    auto const selfParentId = ListId{5};
    auto const projection = buildListTreeProjection(
      std::vector{rt::ListNode{.id = orphanId, .parentId = ListId{999}, .name = "Orphan"},
                  rt::ListNode{.id = selfParentId, .parentId = selfParentId, .name = "Self Parent"}});

    auto const& allTracks = projection.rowsById.at(rt::kAllTracksListId);
    CHECK(allTracks.childIds == std::vector{orphanId, selfParentId});
    CHECK(projection.rowsById.at(orphanId).parentId == ListId{999});
    CHECK(projection.rowsById.at(selfParentId).parentId == selfParentId);
  }

  TEST_CASE("buildListTreeProjection orders children by list id", "[uimodel][unit][library][list]")
  {
    auto const parentId = ListId{2};
    auto const projection = buildListTreeProjection(std::vector{
      rt::ListNode{.id = ListId{30}, .parentId = parentId, .name = "Third in snapshot"},
      rt::ListNode{.id = parentId, .parentId = kInvalidListId, .name = "Parent"},
      rt::ListNode{.id = ListId{10}, .parentId = parentId, .name = "First by id"},
      rt::ListNode{.id = ListId{20}, .parentId = parentId, .name = "Second by id"},
    });

    CHECK(projection.rowsById.at(parentId).childIds == std::vector{ListId{10}, ListId{20}, ListId{30}});
  }
} // namespace ao::uimodel::test
