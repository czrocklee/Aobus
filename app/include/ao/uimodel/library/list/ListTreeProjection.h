// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/ListNode.h>

#include <map>
#include <span>
#include <string>
#include <vector>

namespace ao::uimodel
{
  struct ListTreeProjectionRow final
  {
    ListId id = kInvalidListId;
    ListId parentId = kInvalidListId;
    std::string name;
    rt::ListNodeKind kind = rt::ListNodeKind::Folder;
    std::string localExpression = {};
    std::vector<ListId> childIds = {};
  };

  struct ListTreeProjection final
  {
    std::vector<ListId> rootIds;
    std::map<ListId, ListTreeProjectionRow> rowsById;
  };

  ListTreeProjection buildListTreeProjection(std::span<rt::ListNode const> snapshot);
} // namespace ao::uimodel
