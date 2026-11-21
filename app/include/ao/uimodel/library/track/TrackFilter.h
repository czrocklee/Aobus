// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/completion/QueryExpressionCompleter.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::rt
{
  class CompletionService;
}

namespace ao::uimodel
{
  enum class TrackFilterMode : std::uint8_t
  {
    None,
    Quick,
    Expression,
  };

  struct ResolvedTrackFilter final
  {
    TrackFilterMode mode = TrackFilterMode::None;
    std::string expression{};
  };

  ResolvedTrackFilter resolveTrackFilter(std::string_view rawFilter);

  /// Returns an empty expression for an empty value or a field without a filter variable.
  std::string trackFieldTextEqualityFilter(rt::TrackField field, std::string_view value);

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
