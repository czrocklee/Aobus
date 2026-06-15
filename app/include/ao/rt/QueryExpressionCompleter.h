// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "CompletionResult.h"

#include <cstddef>
#include <optional>
#include <string_view>

namespace ao::rt
{
  class CompletionService;

  class QueryExpressionCompleter final
  {
  public:
    explicit QueryExpressionCompleter(CompletionService& vocabulary);

    std::optional<CompletionResult> complete(std::string_view text,
                                             std::size_t cursor,
                                             std::size_t limit = kCompletionResultLimit);

  private:
    CompletionService& _vocabulary;
  };
} // namespace ao::rt
