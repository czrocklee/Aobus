// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/ListNode.h>

#include <span>
#include <string>
#include <vector>

namespace ao::uimodel
{
  class PresentationTextCatalog;
}

namespace ao::tui
{
  class TuiTextCatalog;
}

namespace ao::tui
{
  struct LibraryNavEntry final
  {
    ListId id{};
    std::string label{};
    std::string detail{};
  };

  std::string listNodeIcon();
  std::string listTitle(ListId listId, std::vector<LibraryNavEntry> const& items);
  std::vector<LibraryNavEntry> makeLibraryNavigation(uimodel::PresentationTextCatalog const& textCatalog,
                                                     TuiTextCatalog const& tuiTextCatalog,
                                                     std::span<rt::ListNode const> lists);
  std::vector<std::string> libraryNavigationLabels(std::vector<LibraryNavEntry> const& items);
} // namespace ao::tui
