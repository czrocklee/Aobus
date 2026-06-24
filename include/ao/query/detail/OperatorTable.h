// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

// Single source of truth for per-Operator attributes (source spelling and the
// predicate class that decides whether an operator yields a boolean). The query
// compilers previously each carried their own parallel switch over Operator;
// routing them through this table means a new operator forces exactly one edit
// here (guarded by the size static_assert below) instead of silently leaving one
// switch stale.

#include <ao/query/Expression.h>

#include <gsl-lite/gsl-lite.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ao::query::detail
{
  enum class OperatorClass : std::uint8_t
  {
    Logical,    // and/or: a predicate iff both operands are predicates
    Boolean,    // =,!=,~,<,<=,>,>=,in: always yields a boolean predicate
    Arithmetic, // +: never a predicate
    Unary,      // not/exists: arity-1, predicate decided structurally by the caller
  };

  struct OperatorInfo
  {
    std::string_view spelling; // canonical source token, no surrounding spaces
    OperatorClass cls;
  };

  // Indexed by the integer value of Operator; the order MUST match the enum.
  inline constexpr auto kOperatorTable = std::to_array<OperatorInfo>({
    {.spelling = "and", .cls = OperatorClass::Logical},
    {.spelling = "or", .cls = OperatorClass::Logical},
    {.spelling = "not", .cls = OperatorClass::Unary},
    {.spelling = "=", .cls = OperatorClass::Boolean},
    {.spelling = "!=", .cls = OperatorClass::Boolean},
    {.spelling = "~", .cls = OperatorClass::Boolean},
    {.spelling = "<", .cls = OperatorClass::Boolean},
    {.spelling = "<=", .cls = OperatorClass::Boolean},
    {.spelling = ">", .cls = OperatorClass::Boolean},
    {.spelling = ">=", .cls = OperatorClass::Boolean},
    {.spelling = "in", .cls = OperatorClass::Boolean},
    {.spelling = "+", .cls = OperatorClass::Arithmetic},
    {.spelling = "?", .cls = OperatorClass::Unary},
  });

  static_assert(kOperatorTable.size() == static_cast<std::size_t>(Operator::Exists) + 1,
                "kOperatorTable must have exactly one entry per Operator enumerator");

  constexpr OperatorInfo const& operatorInfo(Operator op)
  {
    auto const index = static_cast<std::size_t>(op);
    gsl_Expects(index < kOperatorTable.size());
    return kOperatorTable[index];
  }
} // namespace ao::query::detail
