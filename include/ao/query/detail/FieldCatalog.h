// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/query/Expression.h>
#include <ao/query/Field.h>

#include <span>
#include <string_view>

namespace ao::query::detail
{
  struct QueryVariableCompletionSpec final
  {
    VariableType type = VariableType::Metadata;
    Field field = Field::Title;
    std::string_view canonicalName;
    std::span<std::string_view const> aliases;
  };

  std::span<QueryVariableCompletionSpec const> queryVariableCompletionSpecs(VariableType type);

  QueryVariableCompletionSpec const* findQueryVariableCompletionSpec(VariableType type, std::string_view name);
} // namespace ao::query::detail
