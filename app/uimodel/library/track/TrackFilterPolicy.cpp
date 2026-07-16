// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackFilterPolicy.h"

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel::detail
{
  namespace
  {
    struct QuoteState final
    {
      char quote = '\0';
      bool escaped = false;
    };

    bool isSpace(char ch)
    {
      return std::isspace(static_cast<unsigned char>(ch)) != 0;
    }

    void skipSpaces(std::string_view text, std::size_t& position)
    {
      while (position < text.size() && isSpace(text[position]))
      {
        ++position;
      }
    }

    bool startsNotOperator(std::string_view text, std::size_t position)
    {
      auto const isNot = text.size() - position >= 3 &&
                         std::tolower(static_cast<unsigned char>(text[position])) == 'n' &&
                         std::tolower(static_cast<unsigned char>(text[position + 1])) == 'o' &&
                         std::tolower(static_cast<unsigned char>(text[position + 2])) == 't';

      if (!isNot)
      {
        return false;
      }

      auto const end = position + 3;
      return end == text.size() || isSpace(text[end]) || text[end] == '(' || text[end] == '!';
    }

    bool isExpressionVariablePrefix(char ch)
    {
      return ch == '$' || ch == '@' || ch == '#' || ch == '%';
    }

    void advanceQuoteState(QuoteState& state, char ch)
    {
      if (state.quote == '\0')
      {
        if (ch == '\'' || ch == '"')
        {
          state.quote = ch;
        }

        return;
      }

      if (state.escaped)
      {
        state.escaped = false;
      }
      else if (ch == '\\')
      {
        state.escaped = true;
      }
      else if (ch == state.quote)
      {
        state.quote = '\0';
      }
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
      char quote = '\0';
      bool escaped = false;

      for (auto const ch : text)
      {
        if (quote == '\0')
        {
          if (ch == '\'' || ch == '"')
          {
            quote = ch;
          }
          else
          {
            result.push_back(ch);
          }

          continue;
        }

        if (escaped)
        {
          appendEscaped(result, ch);
          escaped = false;
        }
        else if (ch == '\\')
        {
          escaped = true;
        }
        else if (ch == quote)
        {
          quote = '\0';
        }
        else
        {
          result.push_back(ch);
        }
      }

      if (escaped)
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

      if (startsNotOperator(text, position))
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

      if (state.quote == '\0' && isSpace(ch))
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

      if (state.quote == '\0' && isSpace(ch))
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

      if (state.quote == '\0' && isSpace(ch))
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
