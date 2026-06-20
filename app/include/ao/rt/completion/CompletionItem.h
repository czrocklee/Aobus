// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string>

namespace ao::rt
{
  struct CompletionItem final
  {
    std::string displayText;
    std::string insertText;
    std::string detail;
    std::uint32_t rank = 0;
  };
} // namespace ao::rt
