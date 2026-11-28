// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackFilterPolicy.h"

#include <ao/utility/String.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel::detail
{
  namespace
  {
    enum class TermByte : std::uint8_t
    {
      Literal,
      Escaped,
      Syntax,
    };

    struct QuoteState final
    {
      char quote = '\0';
      bool escaped = false;
    };

    void skipSpaces(std::string_view text, std::size_t& position)
    {
      while (position < text.size() && utility::isAsciiWhitespace(text[position]))
      {
        ++position;
      }
    }

    bool startsWithNotOperator(std::string_view text, std::size_t position)
    {
      auto const isNot = text.size() - position >= 3 && utility::toAsciiLower(text[position]) == 'n' &&
                         utility::toAsciiLower(text[position + 1]) == 'o' &&
                         utility::toAsciiLower(text[position + 2]) == 't';

      if (!isNot)
      {
        return false;
      }

      auto const end = position + 3;
      return end == text.size() || utility::isAsciiWhitespace(text[end]) || text[end] == '(' || text[end] == '!';
    }

    bool isExpressionVariablePrefix(char ch)
    {
      return ch == '$' || ch == '@' || ch == '#' || ch == '%';
    }

    TermByte advanceQuoteState(QuoteState& state, char ch)
    {
      if (state.quote == '\0')
      {
        if (ch == '\'' || ch == '"')
        {
          state.quote = ch;
          return TermByte::Syntax;
        }

        return TermByte::Literal;
      }

      if (state.escaped)
      {
        state.escaped = false;
        return TermByte::Escaped;
      }

      if (ch == '\\')
      {
        state.escaped = true;
        return TermByte::Syntax;
      }

      if (ch == state.quote)
      {
        state.quote = '\0';
        return TermByte::Syntax;
      }

      return TermByte::Literal;
    }

    void appendEscaped(std::string& output, char ch)
    {
      switch (ch)
      {
        case 'n': output.push_back('\n'); break;
        case 't': output.push_back('\t'); break;
        case 'r': output.push_back('\r'); break;
        case '\\': output.push_back('\\'); break;
        case '\'': output.push_back('\''); break;
        case '"': output.push_back('"'); break;
        default:
          output.push_back('\\');
          output.push_back(ch);
          break;
      }
    }

    std::string decodeQuickFilterTerm(std::string_view text)
    {
      auto result = std::string{};
      result.reserve(text.size());
      auto state = QuoteState{};

      for (auto const ch : text)
      {
        switch (advanceQuoteState(state, ch))
        {
          case TermByte::Literal: result.push_back(ch); break;
          case TermByte::Escaped: appendEscaped(result, ch); break;
          case TermByte::Syntax: break;
        }
      }

      if (state.escaped)
      {
        result.push_back('\\');
      }

      return result;
    }
  } // namespace

  bool isExplicitTrackFilterExpression(std::string_view text)
  {
    std::size_t position = 0;
    skipSpaces(text, position);

    while (position < text.size())
    {
      if (text[position] == '(' || text[position] == '!')
      {
        ++position;
        skipSpaces(text, position);
        continue;
      }

      if (startsWithNotOperator(text, position))
      {
        position += 3;
        skipSpaces(text, position);
        continue;
      }

      break;
    }

    return position < text.size() && isExpressionVariablePrefix(text[position]);
  }

  std::vector<std::string> splitQuickFilterTerms(std::string_view text)
  {
    auto result = std::vector<std::string>{};
    auto optTermBegin = std::optional<std::size_t>{};
    auto state = QuoteState{};

    for (std::size_t index = 0; index < text.size(); ++index)
    {
      auto const ch = text[index];

      if (state.quote == '\0' && utility::isAsciiWhitespace(ch))
      {
        if (optTermBegin)
        {
          if (auto term = decodeQuickFilterTerm(text.substr(*optTermBegin, index - *optTermBegin)); !term.empty())
          {
            result.push_back(std::move(term));
          }

          optTermBegin.reset();
        }

        continue;
      }

      if (!optTermBegin)
      {
        optTermBegin = index;
      }

      advanceQuoteState(state, ch);
    }

    if (optTermBegin)
    {
      if (auto term = decodeQuickFilterTerm(text.substr(*optTermBegin)); !term.empty())
      {
        result.push_back(std::move(term));
      }
    }

    return result;
  }

  std::optional<QuickFilterCompletionToken> analyzeQuickFilterCompletion(std::string_view text, std::size_t cursor)
  {
    if (cursor > text.size())
    {
      return std::nullopt;
    }

    std::size_t tokenBegin = 0;
    auto state = QuoteState{};

    for (std::size_t index = 0; index < cursor; ++index)
    {
      auto const ch = text[index];

      if (state.quote == '\0' && utility::isAsciiWhitespace(ch))
      {
        tokenBegin = index + 1;
        continue;
      }

      advanceQuoteState(state, ch);
    }

    auto tokenEnd = cursor;

    while (tokenEnd < text.size())
    {
      auto const ch = text[tokenEnd];

      if (state.quote == '\0' && utility::isAsciiWhitespace(ch))
      {
        break;
      }

      advanceQuoteState(state, ch);
      ++tokenEnd;
    }

    auto prefix = decodeQuickFilterTerm(text.substr(tokenBegin, cursor - tokenBegin));

    if (prefix.empty())
    {
      return std::nullopt;
    }

    return QuickFilterCompletionToken{
      .replaceBegin = tokenBegin,
      .replaceEnd = tokenEnd,
      .prefix = std::move(prefix),
    };
  }
} // namespace ao::uimodel::detail
