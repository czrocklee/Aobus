// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "detail/CompletionTokenizer.h"
#include "detail/Lexical.h"
#include <ao/query/Completion.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/Parser.h>
#include <ao/query/detail/FieldCatalog.h>
#include <ao/query/detail/FieldResolver.h>
#include <ao/query/detail/Predicate.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ao::query
{
  namespace
  {
    constexpr auto kStringOperators = std::to_array<std::string_view>({"=", "!=", "~", "in", "?"});
    constexpr auto kNumericOperators = std::to_array<std::string_view>({"=", "!=", "<", "<=", ">", ">=", "in", "?"});
    constexpr auto kTagOperators = std::to_array<std::string_view>({"?"});
    constexpr auto kFallbackOperators = std::to_array<std::string_view>({"=", "!=", "in", "?"});
    constexpr auto kLogicalOperators = std::to_array<std::string_view>({"and", "or", "&&", "||"});

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

    bool hasExactAlias(detail::QueryVariableCompletionSpec const& spec, std::string_view prefix)
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

    bool isIdentifierLikeToken(detail::CompletionTokenKind kind)
    {
      using enum detail::CompletionTokenKind;

      switch (kind)
      {
        case Variable:
        case Bareword:
        case BooleanLiteral:
        case IntegerLiteral:
        case UnitLiteral: return true;
        default: return false;
      }
    }

    bool isValuePrefixBoundary(detail::CompletionTokenKind kind)
    {
      using enum detail::CompletionTokenKind;

      switch (kind)
      {
        case RelationalOperator:
        case LogicalOperator:
        case PrefixOperator:
        case PostfixOperator:
        case OpenList:
        case CloseList:
        case OpenGroup:
        case CloseGroup:
        case Comma:
        case Whitespace: return true;
        default: return false;
      }
    }

    detail::CompletionToken const* tokenEndingAt(std::span<detail::CompletionToken const> tokens, std::size_t end)
    {
      auto iter = std::ranges::find(tokens, end, &detail::CompletionToken::end);

      if (iter == tokens.end())
      {
        return nullptr;
      }

      return &*iter;
    }

    detail::CompletionToken const* tokenContaining(std::span<detail::CompletionToken const> tokens,
                                                   std::size_t cursorPosition)
    {
      auto iter = std::ranges::find_if(tokens,
                                       [cursorPosition](detail::CompletionToken token)
                                       { return token.begin <= cursorPosition && cursorPosition < token.end; });

      if (iter == tokens.end())
      {
        return nullptr;
      }

      return &*iter;
    }

    detail::CompletionToken const* previousTokenEndingAt(std::span<detail::CompletionToken const> tokens,
                                                         std::size_t end)
    {
      for (auto const& token : std::views::reverse(tokens))
      {
        if (token.end == end)
        {
          return &token;
        }
      }

      return nullptr;
    }

    detail::CompletionToken const* previousSignificantTokenBefore(std::span<detail::CompletionToken const> tokens,
                                                                  std::size_t end)
    {
      for (auto const& token : std::views::reverse(tokens))
      {
        if (token.end <= end && token.kind != detail::CompletionTokenKind::Whitespace)
        {
          return &token;
        }
      }

      return nullptr;
    }

    std::size_t skipWhitespaceBefore(std::span<detail::CompletionToken const> tokens, std::size_t cursorPosition)
    {
      auto cursor = cursorPosition;

      for (;;)
      {
        auto const* token = previousTokenEndingAt(tokens, cursor);

        if (token == nullptr || token->kind != detail::CompletionTokenKind::Whitespace)
        {
          break;
        }

        cursor = token->begin;
      }

      return cursor;
    }

    bool isSimpleVariableCompletionText(std::string_view text)
    {
      return !text.empty() && detail::isVariableSigil(text.front()) &&
             std::ranges::all_of(text.substr(1), detail::isQueryIdentifierChar);
    }

    bool hasAdjacentIdentifierBefore(std::span<detail::CompletionToken const> tokens, detail::CompletionToken token)
    {
      auto const* previous = previousTokenEndingAt(tokens, token.begin);

      return previous != nullptr && isIdentifierLikeToken(previous->kind);
    }

    bool shouldBlockCompletionAtCursor(std::span<detail::CompletionToken const> tokens, std::size_t cursor)
    {
      auto const* token = tokenContaining(tokens, cursor);

      if (token == nullptr)
      {
        return false;
      }

      if (token->begin < cursor)
      {
        return token->kind != detail::CompletionTokenKind::Whitespace;
      }

      return isIdentifierLikeToken(token->kind);
    }

    bool hasBlockingPartialTailAtCursor(std::string_view text,
                                        std::span<detail::CompletionToken const> tokens,
                                        std::size_t cursor)
    {
      auto const* token = tokenEndingAt(tokens, cursor);

      return token != nullptr && token->kind == detail::CompletionTokenKind::PartialTail &&
             !isSimpleVariableCompletionText(detail::tokenText(text, *token));
    }

    std::optional<QueryCompletionToken> variableCompletionTokenAtCursor(std::string_view text,
                                                                        std::span<detail::CompletionToken const> tokens,
                                                                        std::size_t cursor)
    {
      if (cursor == 0 || cursor > text.size())
      {
        return std::nullopt;
      }

      auto const* token = tokenEndingAt(tokens, cursor);

      if (token == nullptr || (token->kind != detail::CompletionTokenKind::Variable &&
                               token->kind != detail::CompletionTokenKind::PartialTail))
      {
        return std::nullopt;
      }

      auto const value = detail::tokenText(text, *token);

      if (!isSimpleVariableCompletionText(value) || hasAdjacentIdentifierBefore(tokens, *token))
      {
        return std::nullopt;
      }

      return QueryCompletionToken{
        .type = variableTypeForTrigger(value.front()),
        .trigger = value.front(),
        .replaceBegin = token->begin,
        .replaceEnd = cursor,
        .prefix = std::string{value.substr(1)},
      };
    }

    struct ParsedVariable final
    {
      VariableType type = VariableType::Metadata;
      Field field = Field::Title;
      std::size_t begin = 0;
      std::size_t end = 0;
    };

    std::optional<ParsedVariable> parseVariableEndingAt(std::string_view text,
                                                        std::span<detail::CompletionToken const> tokens,
                                                        std::size_t end)
    {
      auto const* token = tokenEndingAt(tokens, end);

      if (token == nullptr || token->kind != detail::CompletionTokenKind::Variable ||
          hasAdjacentIdentifierBefore(tokens, *token))
      {
        return std::nullopt;
      }

      auto const value = detail::tokenText(text, *token);

      if (value.empty() || !detail::isVariableSigil(value.front()))
      {
        return std::nullopt;
      }

      auto const type = variableTypeForTrigger(value.front());
      auto const name = isSimpleVariableCompletionText(value) ? value.substr(1) : std::string_view{};
      auto optField = detail::lookupVariableField(type, name);

      if (!optField)
      {
        return std::nullopt;
      }

      return ParsedVariable{.type = type, .field = *optField, .begin = token->begin, .end = token->end};
    }

    struct ParsedOperator final
    {
      std::string_view text;
      std::size_t begin = 0;
      std::size_t end = 0;
    };

    std::optional<ParsedOperator> parseOperatorEndingAt(std::string_view text,
                                                        std::span<detail::CompletionToken const> tokens,
                                                        std::size_t end)
    {
      auto const* token = tokenEndingAt(tokens, end);

      if (token == nullptr)
      {
        return std::nullopt;
      }

      auto const value = detail::tokenText(text, *token);

      if (token->kind == detail::CompletionTokenKind::RelationalOperator)
      {
        return ParsedOperator{.text = equalsInsensitive(value, "in") ? std::string_view{"in"} : value,
                              .begin = token->begin,
                              .end = token->end};
      }

      if (token->kind == detail::CompletionTokenKind::Bareword && equalsInsensitive(value, "in"))
      {
        return ParsedOperator{.text = "in", .begin = token->begin, .end = token->end};
      }

      return std::nullopt;
    }

    std::optional<detail::CompletionToken> findLastUnclosedListOpen(std::span<detail::CompletionToken const> tokens,
                                                                    std::size_t end)
    {
      auto stack = std::vector<detail::CompletionToken>{};

      for (auto const token : tokens)
      {
        if (token.end > end)
        {
          break;
        }

        if (token.kind == detail::CompletionTokenKind::OpenList)
        {
          stack.push_back(token);
        }
        else if (token.kind == detail::CompletionTokenKind::CloseList && !stack.empty())
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

    std::size_t findValuePrefixStart(std::span<detail::CompletionToken const> tokens, std::size_t cursor)
    {
      auto start = cursor;

      for (;;)
      {
        auto const* token = previousTokenEndingAt(tokens, start);

        if (token == nullptr || isValuePrefixBoundary(token->kind))
        {
          break;
        }

        start = token->begin;
      }

      return start;
    }

    bool hasCompletedExpressionEndingAt(std::string_view text,
                                        std::span<detail::CompletionToken const> tokens,
                                        std::size_t end)
    {
      if (end == 0)
      {
        return false;
      }

      if (auto optVariable = parseVariableEndingAt(text, tokens, end); optVariable)
      {
        return optVariable->type == VariableType::Tag;
      }

      auto const expression = text.substr(0, end);

      // parse() validates the same grammar as matchesExpressionSyntax() and additionally builds the
      // AST, returning no value on a grammar mismatch. A single parse() therefore subsumes the
      // syntactic check: a grammar failure (or a future value callback that fails after a syntactic
      // match) leaves parsed empty and degrades to "not a completed expression".
      auto const parsed = parse(expression);

      return parsed && detail::isPredicateExpression(*parsed);
    }

    bool isOperatorCompletionPrefixToken(std::string_view text, detail::CompletionToken token)
    {
      if (token.kind != detail::CompletionTokenKind::RelationalOperator &&
          token.kind != detail::CompletionTokenKind::PrefixOperator &&
          token.kind != detail::CompletionTokenKind::PostfixOperator &&
          token.kind != detail::CompletionTokenKind::Bareword)
      {
        return false;
      }

      auto const prefix = detail::tokenText(text, token);

      if (prefix.empty())
      {
        return false;
      }

      return std::ranges::all_of(prefix,
                                 [](char ch)
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
                                 });
    }

    bool isLogicalOperatorCompletionPrefixToken(detail::CompletionToken token)
    {
      return token.kind == detail::CompletionTokenKind::LogicalOperator ||
             token.kind == detail::CompletionTokenKind::Bareword ||
             token.kind == detail::CompletionTokenKind::BooleanLiteral ||
             token.kind == detail::CompletionTokenKind::IntegerLiteral ||
             token.kind == detail::CompletionTokenKind::UnitLiteral ||
             token.kind == detail::CompletionTokenKind::Unknown;
    }

    std::optional<QueryValueCompletionContext> analyzeValueCompletion(std::string_view text,
                                                                      std::span<detail::CompletionToken const> tokens,
                                                                      std::size_t cursor)
    {
      auto const valueStart = findValuePrefixStart(tokens, cursor);
      auto operatorEnd = skipWhitespaceBefore(tokens, valueStart);
      auto const* operatorPrevious = previousSignificantTokenBefore(tokens, operatorEnd);

      if (operatorPrevious != nullptr &&
          (operatorPrevious->kind == detail::CompletionTokenKind::OpenList ||
           operatorPrevious->kind == detail::CompletionTokenKind::Comma) &&
          operatorPrevious->end == operatorEnd)
      {
        auto optListOpen = findLastUnclosedListOpen(tokens, operatorEnd);

        if (!optListOpen)
        {
          return std::nullopt;
        }

        operatorEnd = skipWhitespaceBefore(tokens, optListOpen->begin);
      }

      auto optOperator = parseOperatorEndingAt(text, tokens, operatorEnd);

      if (!optOperator || optOperator->text == "?")
      {
        return std::nullopt;
      }

      if (valueStart == cursor && optOperator->end == cursor &&
          (optOperator->text == "<" || optOperator->text == ">" || optOperator->text == "in"))
      {
        return std::nullopt;
      }

      auto const lvalueEnd = skipWhitespaceBefore(tokens, optOperator->begin);
      auto optVariable = parseVariableEndingAt(text, tokens, lvalueEnd);

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

    std::optional<QueryOperatorCompletionContext> analyzeOperatorCompletion(
      std::string_view text,
      std::span<detail::CompletionToken const> tokens,
      std::size_t cursor)
    {
      auto prefixStart = cursor;
      auto lvalueEnd = skipWhitespaceBefore(tokens, prefixStart);

      if (auto const* token = previousTokenEndingAt(tokens, cursor);
          token != nullptr && isOperatorCompletionPrefixToken(text, *token))
      {
        prefixStart = token->begin;
        lvalueEnd = skipWhitespaceBefore(tokens, prefixStart);
      }

      auto optVariable = parseVariableEndingAt(text, tokens, lvalueEnd);

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

    std::optional<QueryLogicalOperatorCompletionContext> analyzeLogicalOperatorCompletion(
      std::string_view text,
      std::span<detail::CompletionToken const> tokens,
      std::size_t cursor)
    {
      auto prefixStart = cursor;
      auto expressionEnd = skipWhitespaceBefore(tokens, prefixStart);

      if (auto const* token = previousTokenEndingAt(tokens, cursor);
          token != nullptr && isLogicalOperatorCompletionPrefixToken(*token))
      {
        prefixStart = token->begin;
        expressionEnd = skipWhitespaceBefore(tokens, prefixStart);
      }

      if (!hasCompletedExpressionEndingAt(text, tokens, expressionEnd))
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
    if (cursor > text.size())
    {
      return std::nullopt;
    }

    auto const tokens = detail::tokenizeCompletionQuery(text);
    auto const prefixTokens = detail::tokenizeCompletionQuery(text.substr(0, cursor));

    if (shouldBlockCompletionAtCursor(tokens, cursor) || hasBlockingPartialTailAtCursor(text, prefixTokens, cursor))
    {
      return std::nullopt;
    }

    if (auto optToken = variableCompletionTokenAtCursor(text, prefixTokens, cursor); optToken)
    {
      return QueryCompletionContext{*optToken};
    }

    if (auto optValueContext = analyzeValueCompletion(text, prefixTokens, cursor); optValueContext)
    {
      return QueryCompletionContext{*optValueContext};
    }

    if (auto optLogicalOperatorContext = analyzeLogicalOperatorCompletion(text, prefixTokens, cursor);
        optLogicalOperatorContext)
    {
      return QueryCompletionContext{*optLogicalOperatorContext};
    }

    if (auto optOperatorContext = analyzeOperatorCompletion(text, prefixTokens, cursor); optOperatorContext)
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
    auto const specs = detail::queryVariableCompletionSpecs(type);

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

  std::vector<QueryVariableSummary> queryVariableSummaries(VariableType type)
  {
    auto summaries = std::vector<QueryVariableSummary>{};
    auto const specs = detail::queryVariableCompletionSpecs(type);
    summaries.reserve(specs.size());

    for (auto const& spec : specs)
    {
      summaries.push_back(QueryVariableSummary{
        .type = spec.type,
        .field = spec.field,
        .canonicalName = spec.canonicalName,
        .aliases = spec.aliases,
      });
    }

    return summaries;
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
        case Field::DiscTotal:
        case Field::MovementNumber:
        case Field::MovementTotal: operators = std::span{kNumericOperators}; break;
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
