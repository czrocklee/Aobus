// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/list/SmartListExpression.h>

#include <format>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  std::string formatSmartListExpressionDisplayText(std::string_view expression)
  {
    return expression.empty() ? "(none)" : std::string{expression};
  }

  std::string combineSmartListEffectiveExpression(std::string_view parent, std::string_view local)
  {
    if (parent.empty())
    {
      return std::string{local};
    }

    if (local.empty())
    {
      return std::string{parent};
    }

    return std::format("({}) and ({})", parent, local);
  }
} // namespace ao::uimodel
