// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "LibraryNavigation.h"

#include "TuiTextCatalog.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
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
                               i18n::MessageCatalog const& textCatalog,
                               uimodel::ListTreeProjectionRow const& row,
                               std::size_t const depth)
    {
      if (row.id == rt::kAllTracksListId)
      {
        items.push_back(LibraryNavEntry{
          .id = row.id, .label = row.name, .detail = tuiChromeText(textCatalog, i18n::MessageId::TuiLibraryDetail)});
        return;
      }

      auto const displayName =
        row.name.empty() ? std::string{i18n::requiredText(textCatalog, i18n::MessageId::LibraryUnnamedList)} : row.name;
      auto label = std::string(depth * 2, ' ');
      label.append(listNodeIcon());
      label.push_back(' ');
      label.append(displayName);

      items.push_back(LibraryNavEntry{
        .id = row.id,
        .label = std::move(label),
        .detail = row.localExpression.empty() ? std::string{} : std::format("[{}]", row.localExpression),
      });
    }
  } // namespace

  std::string listNodeIcon()
  {
    return "[L]";
  }

  std::string listTitle(ListId const listId, std::vector<LibraryNavEntry> const& items)
  {
    auto const it = std::ranges::find(items, listId, &LibraryNavEntry::id);

    if (it != items.end())
    {
      return it->label;
    }

    auto const allTracks = std::ranges::find(items, rt::kAllTracksListId, &LibraryNavEntry::id);
    return allTracks == items.end() ? std::string{} : allTracks->label;
  }

  std::vector<LibraryNavEntry> makeLibraryNavigation(i18n::MessageCatalog const& textCatalog,
                                                     std::span<rt::ListNode const> const lists)
  {
    auto const projection = uimodel::buildListTreeProjection(textCatalog, lists);
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
      appendNavigationEntry(items, textCatalog, row, current.depth);

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
