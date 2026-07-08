// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "LibraryNavigation.h"

#include <ao/CoreIds.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/VirtualListIds.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    std::size_t depthOf(rt::ListNode const& node, std::vector<rt::ListNode> const& lists)
    {
      std::size_t depth = 0;
      auto parentId = node.parentId;
      std::size_t visited = 0;

      while (parentId != kInvalidListId && visited < lists.size())
      {
        auto const it = std::ranges::find(lists, parentId, &rt::ListNode::id);

        if (it == lists.end())
        {
          break;
        }

        ++depth;
        ++visited;
        parentId = it->parentId;
      }

      return depth;
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

  std::vector<LibraryNavEntry> makeLibraryNavigation(std::vector<rt::ListNode> const& lists)
  {
    auto items = std::vector<LibraryNavEntry>{};
    items.reserve(lists.size() + 1);
    items.push_back(LibraryNavEntry{
      .id = rt::kAllTracksListId,
      .label = "All Tracks",
      .detail = "library",
      .completionText = "All Tracks",
    });

    auto sorted = lists;
    std::ranges::sort(sorted,
                      [](rt::ListNode const& lhs, rt::ListNode const& rhs)
                      {
                        if (lhs.parentId != rhs.parentId)
                        {
                          return lhs.parentId.raw() < rhs.parentId.raw();
                        }

                        return lhs.name < rhs.name;
                      });

    for (auto const& node : sorted)
    {
      auto const completionText = node.name.empty() ? std::string{"<Unnamed List>"} : node.name;
      auto label = std::string(depthOf(node, sorted) * 2, ' ');
      label.append(listNodeIcon(node.kind));
      label.push_back(' ');
      label.append(completionText);

      items.push_back(LibraryNavEntry{
        .id = node.id,
        .label = std::move(label),
        .detail = node.smartExpression.empty() ? std::string{} : std::format("[{}]", node.smartExpression),
        .completionText = completionText,
      });
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
