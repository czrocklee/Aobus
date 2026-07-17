// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/CoreIds.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    enum class ParentVisitState : std::uint8_t
    {
      Unvisited,
      Visiting,
      Done,
    };

    void breakParentCycles(std::map<ListId, ListId>& effectiveParentIds)
    {
      auto visitStates = std::map<ListId, ParentVisitState>{};

      for (auto const& entry : effectiveParentIds)
      {
        visitStates.emplace(entry.first, ParentVisitState::Unvisited);
      }

      // Each parent is another map key or the root; rewriting values cannot invalidate iteration.
      for (auto const& entry : effectiveParentIds)
      {
        if (visitStates.at(entry.first) != ParentVisitState::Unvisited)
        {
          continue;
        }

        auto path = std::vector<ListId>{};
        auto currentId = entry.first;

        while (currentId != rt::kAllTracksListId && visitStates.at(currentId) == ParentVisitState::Unvisited)
        {
          visitStates.at(currentId) = ParentVisitState::Visiting;
          path.push_back(currentId);
          currentId = effectiveParentIds.at(currentId);
        }

        if (currentId != rt::kAllTracksListId && visitStates.at(currentId) == ParentVisitState::Visiting)
        {
          if (auto const cycleBegin = std::ranges::find(path, currentId); cycleBegin != path.end())
          {
            auto const cycleRoot = *std::ranges::min_element(cycleBegin, path.end());
            effectiveParentIds.at(cycleRoot) = rt::kAllTracksListId;
          }
        }

        for (auto const id : path)
        {
          visitStates.at(id) = ParentVisitState::Done;
        }
      }
    }

    std::map<ListId, ListId> effectiveParentIds(ListTreeProjection const& projection)
    {
      auto result = std::map<ListId, ListId>{};

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
        result.emplace(id, parentId);
      }

      breakParentCycles(result);

      return result;
    }
  } // namespace

  ListTreeProjection buildListTreeProjection(std::span<rt::ListNode const> snapshot)
  {
    auto projection = ListTreeProjection{};

    projection.rowsById.emplace(rt::kAllTracksListId,
                                ListTreeProjectionRow{.id = rt::kAllTracksListId,
                                                      .parentId = kInvalidListId,
                                                      .name = "All Tracks",
                                                      .kind = rt::ListNodeKind::Folder});
    projection.rootIds.push_back(rt::kAllTracksListId);

    for (auto const& node : snapshot)
    {
      projection.rowsById.emplace(node.id,
                                  ListTreeProjectionRow{.id = node.id,
                                                        .parentId = node.parentId,
                                                        .name = node.name,
                                                        .kind = node.kind,
                                                        .localExpression = node.smartExpression});
    }

    for (auto const& [id, parentId] : effectiveParentIds(projection))
    {
      projection.rowsById.at(id).parentId = parentId;
      projection.rowsById.at(parentId).childIds.push_back(id);
    }

    return projection;
  }
} // namespace ao::uimodel
