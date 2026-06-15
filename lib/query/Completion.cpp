// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Completion.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/FieldCatalog.h>
#include <ao/query/Parser.h>
#include <ao/query/Predicate.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ao::query
{
  namespace
  {
    constexpr auto kStringOperators = std::array<std::string_view, 5>{"=", "!=", "~", "in", "?"};
    constexpr auto kNumericOperators = std::array<std::string_view, 8>{"=", "!=", "<", "<=", ">", ">=", "in", "?"};
    constexpr auto kTagOperators = std::array<std::string_view, 1>{"?"};
    constexpr auto kFallbackOperators = std::array<std::string_view, 4>{"=", "!=", "in", "?"};
    constexpr auto kLogicalOperators = std::array<std::string_view, 4>{"and", "or", "&&", "||"};

    bool isIdentifierChar(char ch)
    {
      auto const uch = static_cast<unsigned char>(ch);
      return std::isalnum(uch) != 0 || ch == '_';
    }

    bool isWhitespace(char ch)
    {
      return std::isspace(static_cast<unsigned char>(ch)) != 0;
    }

    bool isOperatorPrefixChar(char ch)
    {
      switch (ch)
      {
        case '!':
        case '<':
        case '>':
        case '=':
        case '~':
        case '?': return true;
        default: return ch == 'i' || ch == 'I' || ch == 'n' || ch == 'N';
      }
    }

    bool isLogicalOperatorPrefixChar(char ch)
    {
      return isIdentifierChar(ch) || ch == '&' || ch == '|';
    }

    char lowerAscii(char ch)
    {
      return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    bool startsWithInsensitive(std::string_view candidate, std::string_view prefix)
    {
      if (prefix.size() > candidate.size())
      {
        return false;
      }

      return std::equal(prefix.begin(),
                        prefix.end(),
                        candidate.begin(),
                        [](char lhs, char rhs) { return lowerAscii(lhs) == lowerAscii(rhs); });
    }

    bool equalsInsensitive(std::string_view lhs, std::string_view rhs)
    {
      return lhs.size() == rhs.size() && startsWithInsensitive(lhs, rhs);
    }

    bool hasOpenQuoteBefore(std::string_view text, std::size_t cursor)
    {
      auto quote = char{};
      auto escaped = false;

      for (auto idx = std::size_t{0}; idx < cursor; ++idx)
      {
        auto const ch = text[idx];

        if (quote != '\0')
        {
          if (escaped)
          {
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

          continue;
        }

        if (ch == '"' || ch == '\'')
        {
          quote = ch;
        }
      }

      return quote != '\0';
    }

    VariableType variableTypeForTrigger(char trigger)
    {
      switch (trigger)
      {
        case '$': return VariableType::Metadata;
        case '@': return VariableType::Property;
        case '#': return VariableType::Tag;
        case '%': return VariableType::Custom;
        default: return VariableType::Metadata;
      }
    }

    bool hasExactAlias(QueryVariableCompletionSpec const& spec, std::string_view prefix)
    {
      return std::ranges::any_of(
        spec.aliases, [prefix](std::string_view alias) { return equalsInsensitive(alias, prefix); });
    }

    bool containsCanonical(std::span<QueryVariableCompletionMatch const> matches, std::string_view canonicalName)
    {
      return std::ranges::any_of(matches,
                                 [canonicalName](QueryVariableCompletionMatch const& match)
                                 { return match.canonicalName == canonicalName; });
    }

    std::size_t skipWhitespaceBefore(std::string_view text, std::size_t pos)
    {
      while (pos > 0 && isWhitespace(text[pos - 1]))
      {
        --pos;
      }

      return pos;
    }

    std::optional<QueryCompletionToken> variableCompletionTokenAtCursor(std::string_view text, std::size_t cursor)
    {
      if (cursor == 0 || cursor > text.size() || hasOpenQuoteBefore(text, cursor))
      {
        return std::nullopt;
      }

      if (cursor < text.size() && isIdentifierChar(text[cursor]))
      {
        return std::nullopt;
      }

      auto tokenStart = cursor;

      while (tokenStart > 0 && isIdentifierChar(text[tokenStart - 1]))
      {
        --tokenStart;
      }

      if (tokenStart == 0)
      {
        return std::nullopt;
      }

      auto const trigger = text[tokenStart - 1];

      if (trigger != '$' && trigger != '@' && trigger != '#' && trigger != '%')
      {
        return std::nullopt;
      }

      if (tokenStart > 1 && isIdentifierChar(text[tokenStart - 2]))
      {
        return std::nullopt;
      }

      return QueryCompletionToken{
        .type = variableTypeForTrigger(trigger),
        .trigger = trigger,
        .replaceBegin = tokenStart - 1,
        .replaceEnd = cursor,
        .prefix = std::string{text.substr(tokenStart, cursor - tokenStart)},
      };
    }

    std::optional<std::size_t> findOpeningQuote(std::string_view text, std::size_t quoteEnd)
    {
      auto const quote = text[quoteEnd];

      if (quote != '"' && quote != '\'')
      {
        return std::nullopt;
      }

      for (auto pos = quoteEnd; pos > 0;)
      {
        --pos;

        if (text[pos] == quote)
        {
          auto backslashCount = std::size_t{0};

          for (auto slashPos = pos; slashPos > 0 && text[slashPos - 1] == '\\'; --slashPos)
          {
            ++backslashCount;
          }

          if (backslashCount % 2 == 0)
          {
            return pos;
          }
        }
      }

      return std::nullopt;
    }

    struct ParsedVariable final
    {
      VariableType type = VariableType::Metadata;
      Field field = Field::Title;
      std::size_t begin = 0;
      std::size_t end = 0;
    };

    std::optional<Field> resolveVariable(VariableType type, std::string_view name)
    {
      try
      {
        return resolveVariableField(type, name);
      }
      catch (std::exception const&)
      {
        return std::nullopt;
      }
    }

    std::optional<ParsedVariable> parseUnquotedVariableEndingAt(std::string_view text, std::size_t end)
    {
      if (end == 0 || !isIdentifierChar(text[end - 1]))
      {
        return std::nullopt;
      }

      auto nameBegin = end;

      while (nameBegin > 0 && isIdentifierChar(text[nameBegin - 1]))
      {
        --nameBegin;
      }

      if (nameBegin == 0)
      {
        return std::nullopt;
      }

      auto const trigger = text[nameBegin - 1];

      if (trigger != '$' && trigger != '@' && trigger != '#' && trigger != '%')
      {
        return std::nullopt;
      }

      if (nameBegin > 1 && isIdentifierChar(text[nameBegin - 2]))
      {
        return std::nullopt;
      }

      auto const type = variableTypeForTrigger(trigger);
      auto optField = resolveVariable(type, text.substr(nameBegin, end - nameBegin));

      if (!optField)
      {
        return std::nullopt;
      }

      return ParsedVariable{.type = type, .field = *optField, .begin = nameBegin - 1, .end = end};
    }

    std::optional<ParsedVariable> parseQuotedVariableEndingAt(std::string_view text, std::size_t end)
    {
      if (end == 0 || (text[end - 1] != '"' && text[end - 1] != ']'))
      {
        return std::nullopt;
      }

      auto const quoteEnd = [&] -> std::size_t
      {
        if (text[end - 1] == '"')
        {
          return end - 1;
        }

        if (end >= 2 && text[end - 2] == '"')
        {
          return end - 2;
        }

        return end;
      }();

      if (quoteEnd >= end)
      {
        return std::nullopt;
      }

      auto optQuoteBegin = findOpeningQuote(text, quoteEnd);

      if (!optQuoteBegin)
      {
        return std::nullopt;
      }

      auto triggerPos = *optQuoteBegin;

      if (text[end - 1] == ']')
      {
        if (*optQuoteBegin == 0 || text[*optQuoteBegin - 1] != '[')
        {
          return std::nullopt;
        }

        triggerPos = *optQuoteBegin - 1;
      }

      if (triggerPos == 0)
      {
        return std::nullopt;
      }

      auto const trigger = text[triggerPos - 1];

      if (trigger != '#' && trigger != '%')
      {
        return std::nullopt;
      }

      if (triggerPos > 1 && isIdentifierChar(text[triggerPos - 2]))
      {
        return std::nullopt;
      }

      auto const type = variableTypeForTrigger(trigger);
      auto optField = resolveVariable(type, {});

      if (!optField)
      {
        return std::nullopt;
      }

      return ParsedVariable{.type = type, .field = *optField, .begin = triggerPos - 1, .end = end};
    }

    std::optional<ParsedVariable> parseVariableEndingAt(std::string_view text, std::size_t end)
    {
      if (auto optUnquoted = parseUnquotedVariableEndingAt(text, end); optUnquoted)
      {
        return optUnquoted;
      }

      return parseQuotedVariableEndingAt(text, end);
    }

    struct ParsedOperator final
    {
      std::string_view text;
      std::size_t begin = 0;
      std::size_t end = 0;
    };

    bool hasIdentifierBoundaryBefore(std::string_view text, std::size_t pos)
    {
      return pos == 0 || !isIdentifierChar(text[pos - 1]);
    }

    bool hasIdentifierBoundaryAfter(std::string_view text, std::size_t pos)
    {
      return pos >= text.size() || !isIdentifierChar(text[pos]);
    }

    std::optional<ParsedOperator> parseOperatorEndingAt(std::string_view text, std::size_t end)
    {
      if (end == 0)
      {
        return std::nullopt;
      }

      if (end >= 2 && equalsInsensitive(text.substr(end - 2, 2), "in") && hasIdentifierBoundaryBefore(text, end - 2) &&
          hasIdentifierBoundaryAfter(text, end))
      {
        return ParsedOperator{.text = "in", .begin = end - 2, .end = end};
      }

      auto const twoCharOperators = std::array<std::string_view, 3>{"!=", "<=", ">="};

      if (end >= 2)
      {
        auto const candidate = text.substr(end - 2, 2);

        for (auto const op : twoCharOperators)
        {
          if (candidate == op)
          {
            return ParsedOperator{.text = op, .begin = end - 2, .end = end};
          }
        }
      }

      switch (auto const ch = text[end - 1]; ch)
      {
        case '=':
        case '~':
        case '<':
        case '>': return ParsedOperator{.text = text.substr(end - 1, 1), .begin = end - 1, .end = end};
        default: return std::nullopt;
      }
    }

    std::optional<std::size_t> findLastUnclosedListOpen(std::string_view text, std::size_t end)
    {
      auto quote = char{};
      auto escaped = false;
      auto stack = std::vector<std::size_t>{};

      for (auto idx = std::size_t{0}; idx < end; ++idx)
      {
        auto const ch = text[idx];

        if (quote != '\0')
        {
          if (escaped)
          {
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

          continue;
        }

        if (ch == '"' || ch == '\'')
        {
          quote = ch;
        }
        else if (ch == '[')
        {
          stack.push_back(idx);
        }
        else if (ch == ']' && !stack.empty())
        {
          stack.pop_back();
        }
      }

      if (stack.empty())
      {
        return std::nullopt;
      }

      return stack.back();
    }

    std::size_t findValuePrefixStart(std::string_view text, std::size_t cursor)
    {
      auto start = cursor;

      while (start > 0 && !isWhitespace(text[start - 1]) && text[start - 1] != '[' && text[start - 1] != ',' &&
             text[start - 1] != '!' && text[start - 1] != '<' && text[start - 1] != '>' && text[start - 1] != '=' &&
             text[start - 1] != '~')
      {
        --start;
      }

      return start;
    }

    bool hasCompletedExpressionEndingAt(std::string_view text, std::size_t end)
    {
      if (end == 0)
      {
        return false;
      }

      if (auto optVariable = parseVariableEndingAt(text, end); optVariable)
      {
        return optVariable->type == VariableType::Tag;
      }

      auto const expression = text.substr(0, end);

      if (!matchesExpressionSyntax(expression))
      {
        return false;
      }

      // matchesExpressionSyntax() and parse() share the same grammar, so a syntactic match implies
      // parse() succeeds for today's productions. Guard defensively anyway: this runs on the
      // per-keystroke completion path, and a future value callback that throws after a successful
      // grammar match must degrade to "not a completed expression" rather than escape to the caller.
      try
      {
        return isPredicateExpression(parse(expression));
      }
      catch (std::exception const&)
      {
        return false;
      }
    }

    std::optional<QueryValueCompletionContext> analyzeValueCompletion(std::string_view text, std::size_t cursor)
    {
      auto const valueStart = findValuePrefixStart(text, cursor);
      auto operatorEnd = skipWhitespaceBefore(text, valueStart);

      if (operatorEnd > 0 && (text[operatorEnd - 1] == '[' || text[operatorEnd - 1] == ','))
      {
        auto optListOpen = findLastUnclosedListOpen(text, operatorEnd);

        if (!optListOpen)
        {
          return std::nullopt;
        }

        operatorEnd = skipWhitespaceBefore(text, *optListOpen);
      }

      auto optOperator = parseOperatorEndingAt(text, operatorEnd);

      if (!optOperator || optOperator->text == "?")
      {
        return std::nullopt;
      }

      if (valueStart == cursor && optOperator->end == cursor &&
          (optOperator->text == "<" || optOperator->text == ">" || optOperator->text == "in"))
      {
        return std::nullopt;
      }

      auto const lvalueEnd = skipWhitespaceBefore(text, optOperator->begin);
      auto optVariable = parseVariableEndingAt(text, lvalueEnd);

      if (!optVariable)
      {
        return std::nullopt;
      }

      return QueryValueCompletionContext{
        .field = optVariable->field,
        .replacement =
          QueryCompletionReplacement{
            .replaceBegin = valueStart,
            .replaceEnd = cursor,
            .prefix = std::string{text.substr(valueStart, cursor - valueStart)},
          },
      };
    }

    std::optional<QueryOperatorCompletionContext> analyzeOperatorCompletion(std::string_view text, std::size_t cursor)
    {
      auto prefixStart = cursor;

      while (prefixStart > 0 && isOperatorPrefixChar(text[prefixStart - 1]))
      {
        --prefixStart;
      }

      auto const lvalueEnd = skipWhitespaceBefore(text, prefixStart);
      auto optVariable = parseVariableEndingAt(text, lvalueEnd);

      if (!optVariable)
      {
        return std::nullopt;
      }

      return QueryOperatorCompletionContext{
        .field = optVariable->field,
        .replacement =
          QueryCompletionReplacement{
            .replaceBegin = optVariable->end,
            .replaceEnd = cursor,
            .prefix = std::string{text.substr(prefixStart, cursor - prefixStart)},
          },
      };
    }

    std::optional<QueryLogicalOperatorCompletionContext> analyzeLogicalOperatorCompletion(std::string_view text,
                                                                                          std::size_t cursor)
    {
      auto prefixStart = cursor;

      while (prefixStart > 0 && isLogicalOperatorPrefixChar(text[prefixStart - 1]))
      {
        --prefixStart;
      }

      auto const expressionEnd = skipWhitespaceBefore(text, prefixStart);

      if (!hasCompletedExpressionEndingAt(text, expressionEnd))
      {
        return std::nullopt;
      }

      return QueryLogicalOperatorCompletionContext{
        .replacement =
          QueryCompletionReplacement{
            .replaceBegin = expressionEnd,
            .replaceEnd = cursor,
            .prefix = std::string{text.substr(prefixStart, cursor - prefixStart)},
          },
      };
    }
  } // namespace

  std::optional<QueryCompletionContext> analyzeCompletionContext(std::string_view text, std::size_t cursor)
  {
    if (cursor > text.size() || hasOpenQuoteBefore(text, cursor))
    {
      return std::nullopt;
    }

    if (cursor < text.size() && isIdentifierChar(text[cursor]))
    {
      return std::nullopt;
    }

    if (auto optToken = variableCompletionTokenAtCursor(text, cursor); optToken)
    {
      return QueryCompletionContext{*optToken};
    }

    if (auto optValueContext = analyzeValueCompletion(text, cursor); optValueContext)
    {
      return QueryCompletionContext{*optValueContext};
    }

    if (auto optLogicalOperatorContext = analyzeLogicalOperatorCompletion(text, cursor); optLogicalOperatorContext)
    {
      return QueryCompletionContext{*optLogicalOperatorContext};
    }

    if (auto optOperatorContext = analyzeOperatorCompletion(text, cursor); optOperatorContext)
    {
      return QueryCompletionContext{*optOperatorContext};
    }

    return std::nullopt;
  }

  std::optional<QueryCompletionToken> queryCompletionTokenAtCursor(std::string_view text, std::size_t cursor)
  {
    auto optContext = analyzeCompletionContext(text, cursor);

    if (!optContext)
    {
      return std::nullopt;
    }

    if (auto const* token = std::get_if<QueryCompletionToken>(&*optContext); token != nullptr)
    {
      return *token;
    }

    return std::nullopt;
  }

  std::vector<QueryVariableCompletionMatch> completeQueryVariable(VariableType type, std::string_view prefix)
  {
    auto matches = std::vector<QueryVariableCompletionMatch>{};
    auto const specs = queryVariableCompletionSpecs(type);

    for (auto const& spec : specs)
    {
      if (hasExactAlias(spec, prefix))
      {
        matches.push_back(QueryVariableCompletionMatch{
          .type = spec.type,
          .field = spec.field,
          .canonicalName = spec.canonicalName,
          .kind = QueryVariableCompletionMatchKind::ExactAlias,
        });
      }
    }

    for (auto const& spec : specs)
    {
      if (startsWithInsensitive(spec.canonicalName, prefix) && !containsCanonical(matches, spec.canonicalName))
      {
        matches.push_back(QueryVariableCompletionMatch{
          .type = spec.type,
          .field = spec.field,
          .canonicalName = spec.canonicalName,
          .kind = QueryVariableCompletionMatchKind::CanonicalPrefix,
        });
      }
    }

    return matches;
  }

  std::vector<std::string_view> completeQueryOperator(Field field, std::string_view prefix)
  {
    auto operators = std::span<std::string_view const>{};

    if (isTagField(field))
    {
      operators = std::span{kTagOperators};
    }
    else if (isStringField(field) || isDictionaryField(field))
    {
      operators = std::span{kStringOperators};
    }
    else
    {
      switch (field)
      {
        case Field::Duration:
        case Field::Bitrate:
        case Field::SampleRate:
        case Field::Channels:
        case Field::BitDepth:
        case Field::Year:
        case Field::TrackNumber:
        case Field::TrackTotal:
        case Field::DiscNumber:
        case Field::DiscTotal: operators = std::span{kNumericOperators}; break;
        default: operators = std::span{kFallbackOperators}; break;
      }
    }

    auto matches = std::vector<std::string_view>{};

    for (auto const op : operators)
    {
      if (startsWithInsensitive(op, prefix))
      {
        matches.push_back(op);
      }
    }

    return matches;
  }

  std::vector<std::string_view> completeQueryLogicalOperator(std::string_view prefix)
  {
    auto matches = std::vector<std::string_view>{};

    for (auto const op : kLogicalOperators)
    {
      if (startsWithInsensitive(op, prefix))
      {
        matches.push_back(op);
      }
    }

    return matches;
  }
} // namespace ao::query
