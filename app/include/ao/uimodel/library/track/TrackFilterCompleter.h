// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/QueryExpressionCompleter.h>

#include <cstddef>
#include <optional>
#include <string_view>

namespace ao::rt
{
  class CompletionService;
}

namespace ao::uimodel
{
  class TrackFilterCompleter final
  {
  public:
    explicit TrackFilterCompleter(rt::CompletionService& vocabulary);

    std::optional<rt::CompletionResult> complete(std::string_view text,
                                                 std::size_t cursor,
                                                 std::size_t limit = rt::kCompletionResultLimit);

  private:
    rt::CompletionService& _vocabulary;
    rt::QueryExpressionCompleter _expressionCompleter;
  };
} // namespace ao::uimodel
