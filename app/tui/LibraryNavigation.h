// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>

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

  /// Builds chooser rows from a complete tree projection whose row names are already normalized.
  std::vector<LibraryNavEntry> makeLibraryNavigation(i18n::MessageCatalog const& textCatalog,
                                                     uimodel::ListTreeProjection const& projection);
  std::vector<std::string> libraryNavigationLabels(std::vector<LibraryNavEntry> const& items);
} // namespace ao::tui
