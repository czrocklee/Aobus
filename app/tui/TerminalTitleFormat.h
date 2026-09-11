// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/query/FormatExpression.h>

#include <optional>
#include <string_view>

namespace ao::tui
{
  /// Validates the title preference; an empty expression disables formatting.
  Result<std::optional<query::FormatPlan>> compileTerminalTitleFormat(std::string_view expression);
} // namespace ao::tui
