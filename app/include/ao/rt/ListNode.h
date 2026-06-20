// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Type.h>

#include <cstdint>
#include <string>

namespace ao::rt
{
  enum class ListNodeKind : std::uint8_t
  {
    Folder,
    Manual,
    Smart,
  };

  struct ListNode final
  {
    ListId id{};
    ListId parentId{kInvalidListId};
    std::string name{};
    std::string description{};
    ListNodeKind kind = ListNodeKind::Folder;
    std::string smartExpression{};
  };
} // namespace ao::rt
