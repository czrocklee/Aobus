// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace ao::rt
{
  /**
   * Returns the tag controlled by a List expression only when the parsed AST
   * root is one positive tag variable. This is a derived editing capability,
   * not a persisted List kind.
   */
  std::optional<std::string> writableTagForListExpression(std::string_view expression);

  /// Returns true when any tag variable in the parsed expression names @p tag.
  bool listExpressionReferencesTag(std::string_view expression, std::string_view tag);
} // namespace ao::rt
