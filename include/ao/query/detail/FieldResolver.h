// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>

#include <optional>
#include <string_view>

namespace ao::query::detail
{
  // Resolves a variable expression to its Field. Returns an Error with a
  // diagnostic message for unknown field names or unsupported variable types.
  // Internal to the query compilation layer — callers in the compiler path
  // typically translate the error via throwQueryError(fieldResult.error()).
  Result<Field> resolveVariableField(VariableType type, std::string_view name);
  Result<Field> resolveVariableField(VariableExpression const& variable);

  // Non-diagnostic lookup — returns nullopt for unknown fields instead of
  // an Error. Intended for completion hot paths that degrade gracefully on
  // partial input.
  std::optional<Field> lookupVariableField(VariableType type, std::string_view name);
  std::optional<Field> lookupVariableField(VariableExpression const& variable);
} // namespace ao::query::detail
