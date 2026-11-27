// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

// Canonical serialized spelling and predicate shape for each AST operator.
// Accepted spellings and precedence are defined by the parser grammar.

#include <ao/Contract.h>
#include <ao/query/Expression.h>

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

  struct OperatorDescriptor
  {
    std::string_view spelling; // canonical source token, no surrounding spaces
    OperatorClass cls;
  };

  // Indexed by the integer value of Operator; the order MUST match the enum.
  inline constexpr auto kOperatorTable = std::to_array<OperatorDescriptor>({
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

  constexpr OperatorDescriptor const& operatorDescriptor(Operator op)
  {
    auto const index = static_cast<std::size_t>(op);
    AO_EXPECTS(index < kOperatorTable.size());
    return kOperatorTable[index];
  }
} // namespace ao::query::detail
