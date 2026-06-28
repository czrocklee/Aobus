// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Type.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ListNode.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>

#include <span>

namespace ao::uimodel
{
  ListTreeProjection buildListTreeProjection(std::span<rt::ListNode const> snapshot)
  {
    auto projection = ListTreeProjection{};

    projection.rowsById.emplace(rt::kAllTracksListId,
                                ListTreeProjectionRow{.id = rt::kAllTracksListId,
                                                      .parentId = kInvalidListId,
                                                      .name = "All Tracks",
                                                      .isSmart = false,
                                                      .localExpression = {},
                                                      .childIds = {}});
    projection.rootIds.push_back(rt::kAllTracksListId);

    for (auto const& node : snapshot)
    {
      projection.rowsById.emplace(node.id,
                                  ListTreeProjectionRow{.id = node.id,
                                                        .parentId = node.parentId,
                                                        .name = node.name,
                                                        .isSmart = node.kind == rt::ListNodeKind::Smart,
                                                        .localExpression = node.smartExpression,
                                                        .childIds = {}});
    }

    for (auto const& [id, row] : projection.rowsById)
    {
      if (id == rt::kAllTracksListId)
      {
        continue;
      }

      auto const parentId =
        (row.parentId != kInvalidListId && row.parentId != id && projection.rowsById.contains(row.parentId))
          ? row.parentId
          : rt::kAllTracksListId;
      projection.rowsById.at(parentId).childIds.push_back(id);
    }

    return projection;
  }
} // namespace ao::uimodel
