// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/query/Expression.h>
#include <ao/query/Field.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ao::query
{
  struct QueryCompletionToken final
  {
    VariableType type = VariableType::Metadata;
    char trigger = '\0';
    std::size_t replaceBegin = 0;
    std::size_t replaceEnd = 0;
    // Owns its text so the token can safely outlive the source string it was parsed from.
    std::string prefix;
  };

  struct QueryCompletionReplacement final
  {
    std::size_t replaceBegin = 0;
    std::size_t replaceEnd = 0;
    std::string prefix;
  };

  struct QueryOperatorCompletion final
  {
    Field field = Field::Title;
    QueryCompletionReplacement replacement;
  };

  struct QueryValueCompletion final
  {
    Field field = Field::Title;
    QueryCompletionReplacement replacement;
  };

  struct QueryLogicalOperatorCompletion final
  {
    QueryCompletionReplacement replacement;
  };

  using QueryCompletionAnalysis =
    std::variant<QueryCompletionToken, QueryOperatorCompletion, QueryValueCompletion, QueryLogicalOperatorCompletion>;

  enum class QueryVariableCompletionMatchKind : std::uint8_t
  {
    CanonicalPrefix,
    ExactAlias
  };

  struct QueryVariableCompletionMatch final
  {
    VariableType type = VariableType::Metadata;
    Field field = Field::Title;
    std::string_view canonicalName;
    QueryVariableCompletionMatchKind kind = QueryVariableCompletionMatchKind::CanonicalPrefix;
  };

  struct QueryVariableSummary final
  {
    VariableType type = VariableType::Metadata;
    Field field = Field::Title;
    std::string_view canonicalName;
    std::span<std::string_view const> aliases;
  };

  std::optional<QueryCompletionAnalysis> analyzeQueryCompletion(std::string_view text, std::size_t cursor);

  std::optional<QueryCompletionToken> queryCompletionTokenAtCursor(std::string_view text, std::size_t cursor);

  std::vector<QueryVariableCompletionMatch> completeQueryVariable(VariableType type, std::string_view prefix);

  std::vector<QueryVariableSummary> queryVariableSummaries(VariableType type);

  std::vector<std::string_view> completeQueryOperator(Field field, std::string_view prefix);

  std::vector<std::string_view> completeQueryLogicalOperator(std::string_view prefix);
} // namespace ao::query
