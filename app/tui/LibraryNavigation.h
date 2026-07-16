// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/ListNode.h>

#include <string>
#include <vector>

namespace ao::tui
{
  struct LibraryNavEntry final
  {
    ListId id{};
    std::string label{};
    std::string detail{};
  };

  std::string listNodeIcon(rt::ListNodeKind kind);
  std::string listTitle(ListId listId, std::vector<LibraryNavEntry> const& items);
  std::vector<LibraryNavEntry> makeLibraryNavigation(std::vector<rt::ListNode> const& lists);
  std::vector<std::string> libraryNavigationLabels(std::vector<LibraryNavEntry> const& items);
} // namespace ao::tui
