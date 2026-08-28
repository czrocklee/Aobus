// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/track/TrackFilter.h>

#include "TrackFilterPolicy.h"
#include <ao/Contract.h>
#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/rt/TrackField.h>
#include <ao/utility/String.h>

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  namespace
  {
    std::array<std::string, detail::kQuickFilterFields.size()> const& quickSearchVariables()
    {
      static auto const kVariables = []
      {
        auto variables = std::array<std::string, detail::kQuickFilterFields.size()>{};

        for (std::size_t index = 0; index < detail::kQuickFilterFields.size(); ++index)
        {
          variables[index] = rt::trackFieldFilterExpressionVariable(detail::kQuickFilterFields[index]);
          AO_INVARIANT(!variables[index].empty());
        }

        return variables;
      }();

      return kVariables;
    }

    std::string buildQuickTermExpression(std::string_view term)
    {
      auto const quoted = query::serialize(query::ConstantExpression{std::string{term}});
      auto const& variables = quickSearchVariables();
      auto expression = std::string{"("};

      for (std::size_t index = 0; index < variables.size(); ++index)
      {
        if (index > 0)
        {
          expression += " or ";
        }

        expression += std::format("{} ~ {}", variables[index], quoted);
      }

      auto const tagExpression =
        query::serialize(query::VariableExpression{.type = query::VariableType::Tag, .name = std::string{term}});
      expression += std::format(" or {})", tagExpression);

      return expression;
    }
  } // namespace

  ResolvedTrackFilter resolveTrackFilter(std::string_view rawFilter)
  {
    auto const trimmed = utility::trim(rawFilter);

    if (trimmed.empty())
    {
      return ResolvedTrackFilter{};
    }

    if (detail::isExplicitTrackFilterExpression(trimmed))
    {
      return ResolvedTrackFilter{.mode = TrackFilterMode::Expression, .expression = std::string{trimmed}};
    }

    auto const terms = detail::splitQuickFilterTerms(trimmed);

    if (terms.empty())
    {
      return ResolvedTrackFilter{};
    }

    auto expression = buildQuickTermExpression(terms.front());

    for (std::size_t index = 1; index < terms.size(); ++index)
    {
      expression = std::format("({}) and ({})", expression, buildQuickTermExpression(terms[index]));
    }

    return ResolvedTrackFilter{.mode = TrackFilterMode::Quick, .expression = std::move(expression)};
  }
} // namespace ao::uimodel
