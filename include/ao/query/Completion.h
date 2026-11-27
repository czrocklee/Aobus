// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/query/Expression.h>
#include <ao/query/Field.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ao::query
{
  // [replaceBegin, replaceEnd) is a half-open range of UTF-8 byte offsets into the original input.
  // The prefix owns its text and may outlive the input.
  struct QueryCompletionReplacement final
  {
    std::size_t replaceBegin = 0;
    std::size_t replaceEnd = 0;
    std::string prefix;
  };

  struct QueryVariableCompletion final
  {
    VariableType type = VariableType::Metadata;
    QueryCompletionReplacement replacement;
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

  using QueryCompletionAnalysis = std::
    variant<QueryVariableCompletion, QueryOperatorCompletion, QueryValueCompletion, QueryLogicalOperatorCompletion>;

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

  // cursor is a UTF-8 byte offset into text.
  std::optional<QueryCompletionAnalysis> analyzeQueryCompletion(std::string_view text, std::size_t cursor);

  std::vector<QueryVariableCompletionMatch> completeQueryVariable(VariableType type, std::string_view prefix);

  std::vector<std::string_view> completeQueryOperator(Field field, std::string_view prefix);

  std::vector<std::string_view> completeQueryLogicalOperator(std::string_view prefix);
} // namespace ao::query
