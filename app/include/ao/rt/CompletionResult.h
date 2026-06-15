// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CompletionItem.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace ao::rt
{
  inline constexpr std::size_t kCompletionResultLimit = 64;

  struct CompletionResult final
  {
    std::size_t replaceBegin = 0;
    std::size_t replaceEnd = 0;
    std::vector<CompletionItem> items;
  };

  using CompletionProvider =
    std::move_only_function<std::optional<CompletionResult>(std::string_view text, std::size_t cursor)>;
} // namespace ao::rt
