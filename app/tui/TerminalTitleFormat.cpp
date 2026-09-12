// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "TerminalTitleFormat.h"

#include <ao/Error.h>
#include <ao/query/FormatExpression.h>
#include <ao/query/Parser.h>

#include <expected>
#include <optional>
#include <string_view>
#include <utility>

namespace ao::tui
{
  Result<std::optional<query::FormatPlan>> compileTerminalTitleFormat(std::string_view const expression)
  {
    if (expression.empty())
    {
      return std::nullopt;
    }

    auto parsedRes = query::parse(expression);

    if (!parsedRes)
    {
      return std::unexpected{parsedRes.error()};
    }

    auto planRes = query::compileFormat(*parsedRes);

    if (!planRes)
    {
      return std::unexpected{planRes.error()};
    }

    return std::move(*planRes);
  }
} // namespace ao::tui
