// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/track/TrackFilterResolver.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/trim.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel::track
{
  namespace
  {
    bool isQueryableIdentifier(std::string_view value)
    {
      if (value.empty())
      {
        return false;
      }

      auto const isIdentifierStart = [](char ch)
      {
        auto const uch = static_cast<unsigned char>(ch);
        return std::isalpha(uch) != 0 || ch == '_';
      };

      auto const isIdentifierChar = [](char ch)
      {
        auto const uch = static_cast<unsigned char>(ch);
        return std::isalnum(uch) != 0 || ch == '_';
      };

      if (!isIdentifierStart(value.front()))
      {
        return false;
      }

      return std::ranges::all_of(value, isIdentifierChar);
    }

    bool looksLikeExpression(std::string_view value)
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

      if (isQueryableIdentifier(term))
      {
        expression.insert(expression.size() - 1, std::format(" or #{0}", term));
      }

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

    if (looksLikeExpression(trimmed))
    {
      return ResolvedTrackFilter{.mode = TrackFilterMode::Expression, .expression = trimmed};
    }

    auto const terms = splitQuickTerms(trimmed);

    if (terms.empty())
    {
      return ResolvedTrackFilter{};
    }

    auto const optExpression = std::ranges::fold_left_first(terms | std::views::transform(buildQuickTermExpression),
                                                            [](auto const& acc, auto const& next)
                                                            { return std::format("({}) and ({})", acc, next); });

    return ResolvedTrackFilter{.mode = TrackFilterMode::Quick, .expression = optExpression.value_or("")};
  }
} // namespace ao::uimodel::track
