// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ListNavigationModel.h"

#include "ListSearch.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>
#include <ao/utility/UnicodeText.h>

#include <ftxui/component/event.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    struct PendingNavigationRow final
    {
      ListId id = kInvalidListId;
      ListId parentId = kInvalidListId;
      std::size_t depth = 0;
      std::string pathPrefix{};
    };

    std::string navigationSearchKey(std::string_view const value)
    {
      auto keyRes = utility::makeUtf8CaselessKey(value);
      AO_INVARIANT(keyRes, "Validated List navigation text failed Unicode case folding: {}", keyRes.error().message);
      return std::move(*keyRes);
    }

    std::string appendPath(std::string prefix, std::string const& name)
    {
      if (prefix.empty())
      {
        return name;
      }

      prefix.append(" / ");
      prefix.append(name);
      return prefix;
    }

    std::vector<ListId> oldAncestorIds(uimodel::ListTreeProjection const& tree, ListId id)
    {
      auto ancestors = std::vector<ListId>{};
      auto visited = std::set{id};
      auto rowIt = tree.rowsById.find(id);

      while (rowIt != tree.rowsById.end())
      {
        auto const parentId = rowIt->second.parentId;

        if (parentId == kInvalidListId || !visited.insert(parentId).second)
        {
          break;
        }

        ancestors.push_back(parentId);
        rowIt = tree.rowsById.find(parentId);
      }

      return ancestors;
    }

    bool isDescendantOf(uimodel::ListTreeProjection const& tree, ListId id, ListId const ancestorId)
    {
      auto visited = std::set{id};
      auto rowIt = tree.rowsById.find(id);

      while (rowIt != tree.rowsById.end())
      {
        auto const parentId = rowIt->second.parentId;

        if (parentId == ancestorId)
        {
          return true;
        }

        if (parentId == kInvalidListId || !visited.insert(parentId).second)
        {
          break;
        }

        rowIt = tree.rowsById.find(parentId);
      }

      return false;
    }
  } // namespace

  void ListNavigationModel::setTree(uimodel::ListTreeProjection tree, ListId const activeId)
  {
    auto const ancestors = oldAncestorIds(_tree, _cursor);
    _tree = std::move(tree);
    rebuildProjection();

    std::erase_if(_expandedIds, [&](ListId const id) { return !_tree.rowsById.contains(id); });

    if (isNavigable(_cursor))
    {
      expandAncestors(_cursor);
    }
    else
    {
      _cursor = kInvalidListId;

      for (auto const ancestorId : ancestors)
      {
        if (isNavigable(ancestorId))
        {
          _cursor = ancestorId;
          break;
        }
      }

      if (_cursor == kInvalidListId && isNavigable(activeId))
      {
        _cursor = activeId;
      }

      if (_cursor == kInvalidListId && isNavigable(rt::kAllTracksListId))
      {
        _cursor = rt::kAllTracksListId;
      }

      if (_cursor == kInvalidListId)
      {
        if (!_projection.empty())
        {
          _cursor = _projection.front().row.id;
        }
      }

      expandAncestors(_cursor);
    }

    rebuildRows();
    reconcileSearchCursor();
  }

  void ListNavigationModel::reveal(ListId const id)
  {
    if (!isNavigable(id))
    {
      return;
    }

    expandAncestors(id);
    rebuildRows();
    trySelect(id);
  }

  void ListNavigationModel::expandPath(ListId const id)
  {
    if (!isNavigable(id))
    {
      return;
    }

    expandAncestors(id);
    rebuildRows();
  }

  bool ListNavigationModel::trySelect(ListId const id)
  {
    auto const rowIt = std::ranges::find(_rows, id, &ListNavigationRow::id);

    if (rowIt == _rows.end() || !rowIt->matching)
    {
      return false;
    }

    _cursor = id;
    return true;
  }

  void ListNavigationModel::selectVisibleRow(std::int32_t const rowIndex)
  {
    if (_rows.empty())
    {
      return;
    }

    auto const last = static_cast<std::int64_t>(
      std::min<std::size_t>(_rows.size() - 1, static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())));
    auto const target = std::clamp(static_cast<std::int64_t>(rowIndex), std::int64_t{0}, last);
    auto const current = static_cast<std::int64_t>(selectedIndex());
    auto const direction = target < current ? std::int64_t{-1} : std::int64_t{1};

    auto tryDirection = [&](std::int64_t index, std::int64_t const step)
    {
      while (index >= 0 && index <= last)
      {
        if (_rows[static_cast<std::size_t>(index)].matching)
        {
          _cursor = _rows[static_cast<std::size_t>(index)].id;
          return true;
        }

        index += step;
      }

      return false;
    };

    if (!tryDirection(target, direction))
    {
      tryDirection(target, -direction);
    }
  }

  void ListNavigationModel::move(std::int32_t const delta)
  {
    auto candidates = std::vector<ListId>{};
    candidates.reserve(_rows.size());

    for (auto const& row : _rows)
    {
      if (row.matching)
      {
        candidates.push_back(row.id);
      }
    }

    if (candidates.empty())
    {
      return;
    }

    auto const cursorIt = std::ranges::find(candidates, _cursor);
    auto const current = cursorIt == candidates.end() ? std::int64_t{0} : std::distance(candidates.begin(), cursorIt);
    auto const last = static_cast<std::int64_t>(candidates.size() - 1);
    auto const target = std::clamp(current + static_cast<std::int64_t>(delta), std::int64_t{0}, last);

    _cursor = candidates[static_cast<std::size_t>(target)];
  }

  void ListNavigationModel::expand()
  {
    auto const rowIt = std::ranges::find(_rows, _cursor, &ListNavigationRow::id);

    if (rowIt == _rows.end() || !rowIt->matching || !rowIt->hasChildren)
    {
      return;
    }

    if (!_expandedIds.contains(_cursor))
    {
      _expandedIds.insert(_cursor);
      rebuildRows();
      return;
    }

    auto const childIt = std::ranges::find_if(
      _rows, [&](ListNavigationRow const& row) { return row.parentId == _cursor && row.matching; });

    if (childIt != _rows.end())
    {
      _cursor = childIt->id;
    }
  }

  void ListNavigationModel::collapse()
  {
    auto const rowIt = std::ranges::find(_rows, _cursor, &ListNavigationRow::id);

    if (rowIt == _rows.end() || !rowIt->matching)
    {
      return;
    }

    if (rowIt->hasChildren && _expandedIds.erase(_cursor) > 0)
    {
      rebuildRows();
      return;
    }

    trySelect(rowIt->parentId);
  }

  void ListNavigationModel::toggleExpanded(ListId const id)
  {
    if (_search.isActive())
    {
      return;
    }

    auto const rowIt = std::ranges::find(_rows, id, &ListNavigationRow::id);

    if (rowIt == _rows.end() || !rowIt->hasChildren)
    {
      return;
    }

    if (_expandedIds.contains(id))
    {
      _expandedIds.erase(id);

      if (isDescendantOf(_tree, _cursor, id))
      {
        _cursor = id;
      }
    }
    else
    {
      _expandedIds.insert(id);
    }

    rebuildRows();
  }

  bool ListNavigationModel::trySearchEvent(ftxui::Event const& event)
  {
    auto const wasActive = _search.isActive();
    auto const oldQuery = std::string{_search.query()};

    if (!_search.tryHandleEvent(event))
    {
      return false;
    }

    auto const isActive = _search.isActive();

    if (wasActive == isActive && oldQuery == _search.query())
    {
      return true;
    }

    if (wasActive && !isActive)
    {
      expandAncestors(_cursor);
    }

    rebuildRows();
    reconcileSearchCursor();
    return true;
  }

  void ListNavigationModel::clearSearch()
  {
    auto const wasActive = _search.isActive();
    _search.clear();

    if (!wasActive)
    {
      return;
    }

    expandAncestors(_cursor);
    rebuildRows();
  }

  std::int32_t ListNavigationModel::selectedIndex() const
  {
    auto const rowIt = std::ranges::find(_rows, _cursor, &ListNavigationRow::id);

    if (rowIt == _rows.end())
    {
      return -1;
    }

    auto const index = std::distance(_rows.begin(), rowIt);
    return index > std::numeric_limits<std::int32_t>::max() ? -1 : static_cast<std::int32_t>(index);
  }

  std::optional<ListId> ListNavigationModel::activationTarget() const
  {
    auto const rowIt = std::ranges::find(_rows, _cursor, &ListNavigationRow::id);

    if (rowIt == _rows.end() || !rowIt->matching)
    {
      return std::nullopt;
    }

    return rowIt->id;
  }

  bool ListNavigationModel::isNavigable(ListId const id) const
  {
    return id != kInvalidListId && _tree.rowsById.contains(id);
  }

  void ListNavigationModel::expandAncestors(ListId const id)
  {
    auto visited = std::set{id};
    auto rowIt = _tree.rowsById.find(id);

    while (rowIt != _tree.rowsById.end())
    {
      auto const parentId = rowIt->second.parentId;

      if (parentId == kInvalidListId || !visited.insert(parentId).second)
      {
        break;
      }

      if (_tree.rowsById.contains(parentId))
      {
        _expandedIds.insert(parentId);
      }

      rowIt = _tree.rowsById.find(parentId);
    }
  }

  void ListNavigationModel::rebuildProjection()
  {
    _projection.clear();
    _projection.reserve(_tree.rowsById.size());
    auto visited = std::set<ListId>{};
    auto pending = std::vector<PendingNavigationRow>{};
    pending.reserve(_tree.rowsById.size());

    auto projectRoot = [&](ListId const rootId)
    {
      pending.push_back(PendingNavigationRow{.id = rootId});

      while (!pending.empty())
      {
        auto current = std::move(pending.back());
        pending.pop_back();

        if (current.id == kInvalidListId || !visited.insert(current.id).second)
        {
          continue;
        }

        auto const rowIt = _tree.rowsById.find(current.id);

        if (rowIt == _tree.rowsById.end())
        {
          continue;
        }

        auto const& source = rowIt->second;
        auto path = appendPath(std::move(current.pathPrefix), source.name);
        auto nameKey = navigationSearchKey(source.name);
        auto pathKey = navigationSearchKey(path);
        auto const hasChildren = std::ranges::any_of(
          source.childIds,
          [&](ListId const childId)
          { return childId != current.id && childId != kInvalidListId && _tree.rowsById.contains(childId); });

        _projection.push_back(ProjectedNavigationRow{
          .row = ListNavigationRow{.id = current.id,
                                   .parentId = source.parentId,
                                   .name = source.name,
                                   .detail = source.localExpression,
                                   .path = path,
                                   .depth = current.depth,
                                   .hasChildren = hasChildren},
          .traversalParentId = current.parentId,
          .nameKey = std::move(nameKey),
          .pathKey = std::move(pathKey),
        });

        auto const childDepth = current.id == rt::kAllTracksListId ? current.depth : current.depth + 1;

        for (auto const childId : std::views::reverse(source.childIds))
        {
          pending.push_back(
            PendingNavigationRow{.id = childId, .parentId = current.id, .depth = childDepth, .pathPrefix = path});
        }
      }
    };

    for (auto const rootId : _tree.rootIds)
    {
      projectRoot(rootId);
    }

    // A valid shared projection reaches every row from rootIds. Keeping a
    // deterministic fallback prevents malformed snapshots from hiding every
    // remaining List, and the visited set also bounds child cycles.
    for (auto const& [id, source] : _tree.rowsById)
    {
      std::ignore = source;

      if (!visited.contains(id))
      {
        projectRoot(id);
      }
    }
  }

  void ListNavigationModel::rebuildRows()
  {
    ++_revision;
    _search.invalidateMouseRegions();
    _rows.clear();
    _rows.reserve(_projection.size());

    if (!_search.isActive())
    {
      auto visibleById = std::map<ListId, bool>{};

      for (auto const& item : _projection)
      {
        auto const isRoot = item.traversalParentId == kInvalidListId;
        auto const parentIt = visibleById.find(item.traversalParentId);
        auto const isVisible = isRoot || (parentIt != visibleById.end() && parentIt->second &&
                                          _expandedIds.contains(item.traversalParentId));
        visibleById.insert_or_assign(item.row.id, isVisible);

        if (!isVisible)
        {
          continue;
        }

        auto row = item.row;
        row.expanded = row.hasChildren && _expandedIds.contains(row.id);
        row.matching = true;
        _rows.push_back(std::move(row));
      }

      return;
    }

    auto matchingIds = std::set<ListId>{};
    auto traversalParentIds = std::map<ListId, ListId>{};

    for (auto const& item : _projection)
    {
      traversalParentIds.insert_or_assign(item.row.id, item.traversalParentId);

      if (_search.matchesFolded(item.nameKey) || _search.matchesFolded(item.pathKey))
      {
        matchingIds.insert(item.row.id);
      }
    }

    auto includedIds = matchingIds;

    for (auto const matchingId : matchingIds)
    {
      auto visited = std::set{matchingId};
      auto parentIt = traversalParentIds.find(matchingId);

      while (parentIt != traversalParentIds.end() && parentIt->second != kInvalidListId &&
             visited.insert(parentIt->second).second)
      {
        includedIds.insert(parentIt->second);
        parentIt = traversalParentIds.find(parentIt->second);
      }
    }

    auto parentsWithVisibleChildren = std::set<ListId>{};

    for (auto const& item : _projection)
    {
      if (includedIds.contains(item.row.id) && item.traversalParentId != kInvalidListId)
      {
        parentsWithVisibleChildren.insert(item.traversalParentId);
      }
    }

    for (auto const& item : _projection)
    {
      if (!includedIds.contains(item.row.id))
      {
        continue;
      }

      auto row = item.row;
      row.expanded = row.hasChildren && parentsWithVisibleChildren.contains(row.id);
      row.matching = matchingIds.contains(row.id);
      _rows.push_back(std::move(row));
    }
  }

  void ListNavigationModel::reconcileSearchCursor()
  {
    if (!_search.isActive() || activationTarget())
    {
      return;
    }

    auto const rowIt = std::ranges::find(_rows, true, &ListNavigationRow::matching);

    if (rowIt != _rows.end())
    {
      _cursor = rowIt->id;
    }
  }
} // namespace ao::tui
