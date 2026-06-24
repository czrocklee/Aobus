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
   *         syntax failure. Never throws on malformed input.
   */
  Result<Expression> parse(std::string_view expr);

  /**
   * @return true if @p expr matches the expression grammar. Cheaper than parse() (no AST is
   *         built); used by the per-keystroke completion path.
   */
  bool matchesExpressionSyntax(std::string_view expr);
} // namespace ao::query
