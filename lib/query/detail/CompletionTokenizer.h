// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ao::query::detail
{
  enum class CompletionTokenKind : std::uint8_t
  {
    Variable,
    RelationalOperator,
    LogicalOperator,
    PrefixOperator,
    PostfixOperator,
    AddOperator,
    OpenList,
    CloseList,
    OpenGroup,
    CloseGroup,
    Comma,
    RangeDelimiter,
    StringLiteral,
    Bareword,
    BooleanLiteral,
    IntegerLiteral,
    UnitLiteral,
    Whitespace,
    Unknown,
    PartialTail
  };

  struct CompletionToken final
  {
    CompletionTokenKind kind = CompletionTokenKind::Unknown;
    std::size_t begin = 0;
    std::size_t end = 0;
  };

  std::vector<CompletionToken> tokenizeCompletionQuery(std::string_view text);

  std::string_view tokenText(std::string_view text, CompletionToken token);
} // namespace ao::query::detail
