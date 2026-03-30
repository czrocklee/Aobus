// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/Type.h>

#include <string>
#include <vector>

namespace app::gtkmm4::model
{

/**
 * ListKind - Type of list being created.
 */
enum class ListKind
{
  Smart,  // Expression-based filtered list
  Manual  // Explicit TrackId list
};

/**
 * ListDraft - Plain data transfer object for list creation.
 * Populated by NewListDialog and consumed by MainWindow to create lists.
 */
struct ListDraft
{
  ListKind kind = ListKind::Smart;
  std::string name;
  std::string description;
  std::string expression;  // Only used for Smart lists
  std::vector<rs::core::TrackId> trackIds;  // Only used for Manual lists
};

} // namespace app::gtkmm4::model