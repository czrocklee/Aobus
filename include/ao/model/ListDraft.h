// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Type.h>

#include <string>
#include <vector>

namespace ao::model
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
    ListId parentId = ListId{0};
    ListId listId = ListId{0}; // 0 = create, non-zero = update
    std::string name;
    std::string description;
    std::string expression;            // Only used for Smart lists
    std::vector<TrackId> trackIds; // Only used for Manual lists
  };
} // namespace ao::model
