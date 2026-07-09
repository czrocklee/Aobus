// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/uimodel/library/track/TrackFilterResolver.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    bool isExpressionLike(std::string_view value)
    {
      return std::ranges::any_of(value,
                                 [](char ch)
                                 {
                                   switch (ch)
                                   {
                                     case '$':
                                     case '@':
                                     case '#':
                                     case '%':
                                     case '=':
                                     case '!':
                                     case '<':
                                     case '>':
                                     case '~':
                                     case '(':
                                     case ')':
                                     case '&':
                                     case '|':
                                     case '+': return true;
                                     default: return false;
                                   }
                                 });
    }

    std::vector<std::string> splitQuickTerms(std::string_view value)
    {
      auto result = std::vector<std::string>{};
      auto current = std::string{};
      char quote = '\0';

      for (auto const ch : value)
      {
        if (quote != '\0')
        {
          if (ch == quote)
          {
            quote = '\0';
          }
          else
          {
            current.push_back(ch);
          }

          continue;
        }

        if (ch == '\'' || ch == '"')
        {
          quote = ch;
          continue;
        }

        if (std::isspace(static_cast<unsigned char>(ch)) != 0)
        {
          if (!current.empty())
          {
            result.push_back(std::move(current));
            current.clear();
          }

          continue;
        }

        current.push_back(ch);
      }

      if (!current.empty())
      {
        result.push_back(std::move(current));
      }

      return result;
    }

    std::string quoteExpressionString(std::string_view value)
    {
      if (!value.contains('"'))
      {
        return std::format("\"{}\"", value);
      }

      if (!value.contains('\''))
      {
        return std::format("'{}'", value);
      }

      auto sanitized = std::string{value};
      std::ranges::replace(sanitized, '"', '\'');
      return std::format("\"{}\"", sanitized);
    }

    std::string buildQuickTermExpression(std::string_view term)
    {
      auto const quoted = quoteExpressionString(term);
      auto expression = std::format("($title ~ {0} or $artist ~ {0} or $album ~ {0} or $albumArtist ~ {0} or $genre ~ "
                                    "{0} or $composer ~ {0} or $work ~ {0})",
                                    quoted);

      auto const tagExpression =
        query::serialize(query::VariableExpression{.type = query::VariableType::Tag, .name = std::string{term}});
      expression.insert(expression.size() - 1, std::format(" or {}", tagExpression));

      return expression;
    }
  } // namespace

  ResolvedTrackFilter resolveTrackFilterExpression(std::string_view rawFilter)
  {
    auto const trimmed = boost::algorithm::trim_copy_if(std::string{rawFilter}, boost::algorithm::is_space());

    if (trimmed.empty())
    {
      return ResolvedTrackFilter{};
    }

    if (isExpressionLike(trimmed))
    {
      return ResolvedTrackFilter{.mode = TrackFilterMode::Expression, .expression = trimmed};
    }

    auto const terms = splitQuickTerms(trimmed);

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
