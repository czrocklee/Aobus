// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <string>
#include <string_view>

namespace ao::uimodel
{
  std::string formatSmartListExpressionDisplayText(std::string_view expression);

  std::string combineSmartListEffectiveExpression(std::string_view parent, std::string_view local);
} // namespace ao::uimodel
