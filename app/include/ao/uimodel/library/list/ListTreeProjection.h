// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/ListNode.h>

#include <map>
#include <span>
#include <string>
#include <vector>

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::uimodel
{
  struct ListTreeProjectionRow final
  {
    ListId id = kInvalidListId;
    ListId parentId = kInvalidListId;
    std::string name;
    bool isSystem = false;
    std::string localExpression = {};
    std::vector<ListId> childIds = {};
  };

  struct ListTreeProjection final
  {
    std::vector<ListId> rootIds;
    std::map<ListId, ListTreeProjectionRow> rowsById;
  };

  // Follow projected row.parentId links nearest first, excluding the starting ID and stopping before a repeat.
  // A final dangling parent ID is included: it may exist in a replacement tree even if not in this one.
  std::vector<ListId> listTreeAncestorIds(ListTreeProjection const& projection, ListId id);

  ListTreeProjection buildListTreeProjection(i18n::MessageCatalog const& textCatalog,
                                             std::span<rt::ListNode const> snapshot);
} // namespace ao::uimodel
