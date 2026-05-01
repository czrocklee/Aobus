// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/Type.h>

#include <string>
#include <vector>

namespace rs::model
{
  /**
   * ListKind - Type of list being created.
   */
  enum class ListKind
  {
    Smart, // Expression-based filtered list
    Manual // Explicit TrackId list
  };

  /**
   * ListDraft - Plain data transfer object for list creation.
   * Populated by SmartListDialog and consumed by MainWindow to create or update lists.
   */
  struct ListDraft final
  {
    ListKind kind = ListKind::Smart;
    rs::ListId parentId = rs::ListId{0};
    rs::ListId listId = rs::ListId{0}; // 0 = create, non-zero = update
    std::string name;
    std::string description;
    std::string expression;            // Only used for Smart lists
    std::vector<rs::TrackId> trackIds; // Only used for Manual lists
  };
} // namespace rs::model
