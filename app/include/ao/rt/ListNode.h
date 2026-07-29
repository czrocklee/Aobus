// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <string>

namespace ao::rt
{
  struct ListNode final
  {
    ListId id{};
    ListId parentId{kInvalidListId};
    std::string name{};
    std::string description{};
    std::string expression{};
  };
} // namespace ao::rt
