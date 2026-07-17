// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "LibraryNavigation.h"

#include <ao/CoreIds.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    struct PendingNavigationRow final
    {
      ListId id = kInvalidListId;
      std::size_t depth = 0;
    };

    void appendNavigationEntry(std::vector<LibraryNavEntry>& items,
                               uimodel::ListTreeProjectionRow const& row,
                               std::size_t const depth)
    {
      if (row.id == rt::kAllTracksListId)
      {
        items.push_back(LibraryNavEntry{.id = row.id, .label = row.name, .detail = "library"});
        return;
      }

      auto const displayName = row.name.empty() ? std::string{"<Unnamed List>"} : row.name;
      auto label = std::string(depth * 2, ' ');
      label.append(listNodeIcon(row.kind));
      label.push_back(' ');
      label.append(displayName);

      items.push_back(LibraryNavEntry{
        .id = row.id,
        .label = std::move(label),
        .detail = row.localExpression.empty() ? std::string{} : std::format("[{}]", row.localExpression),
      });
    }
  } // namespace

  std::string listNodeIcon(rt::ListNodeKind const kind)
  {
    switch (kind)
    {
      case rt::ListNodeKind::Folder: return "[+]";
      case rt::ListNodeKind::Manual: return "[#]";
      case rt::ListNodeKind::Smart: return "[?]";
    }

    return "[ ]";
  }

  std::string listTitle(ListId const listId, std::vector<LibraryNavEntry> const& items)
  {
    auto const it = std::ranges::find(items, listId, &LibraryNavEntry::id);
    return it == items.end() ? std::string{"All Tracks"} : it->label;
  }

  std::vector<LibraryNavEntry> makeLibraryNavigation(std::span<rt::ListNode const> const lists)
  {
    auto const projection = uimodel::buildListTreeProjection(lists);
    auto items = std::vector<LibraryNavEntry>{};
    items.reserve(projection.rowsById.size());

    auto pending = std::vector<PendingNavigationRow>{};
    pending.reserve(projection.rowsById.size());

    for (auto const rootId : std::views::reverse(projection.rootIds))
    {
      pending.push_back(PendingNavigationRow{.id = rootId});
    }

    auto visited = std::set<ListId>{};

    while (!pending.empty())
    {
      auto const current = pending.back();
      pending.pop_back();

      if (!visited.insert(current.id).second)
      {
        continue;
      }

      auto const rowIt = projection.rowsById.find(current.id);

      if (rowIt == projection.rowsById.end())
      {
        continue;
      }

      auto const& row = rowIt->second;
      appendNavigationEntry(items, row, current.depth);

      auto const childDepth = row.id == rt::kAllTracksListId ? current.depth : current.depth + 1;

      for (auto const childId : std::views::reverse(row.childIds))
      {
        pending.push_back(PendingNavigationRow{.id = childId, .depth = childDepth});
      }
    }

    return items;
  }

  std::vector<std::string> libraryNavigationLabels(std::vector<LibraryNavEntry> const& items)
  {
    auto labels = std::vector<std::string>{};
    labels.reserve(items.size());

    for (auto const& item : items)
    {
      labels.push_back(item.detail.empty() ? item.label : std::format("{} {}", item.label, item.detail));
    }

    return labels;
  }
} // namespace ao::tui
