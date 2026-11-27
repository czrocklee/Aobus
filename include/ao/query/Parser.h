// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/query/Expression.h>

#include <string_view>

namespace ao::query
{
  /**
   * Parse a query/format expression into an AST.
   *
   * @return the parsed Expression, or an Error{Code::FormatRejected, ...} describing the
   *         syntax or complexity-admission failure. Never throws on malformed input.
   * Text is limited to 65536 bytes, 512 structural tokens outside scalar lists,
   * and 64 nested groups before recursive AST construction.
   */
  Result<Expression> parse(std::string_view expr);

  /**
   * @return true if @p expr passes complexity admission and matches the expression grammar.
   *         Does not build an AST or check predicate or format semantics.
   */
  bool matchesExpressionSyntax(std::string_view expr);
} // namespace ao::query
