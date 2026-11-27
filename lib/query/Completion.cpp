// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Completion.h>

#include "detail/CompletionPredicate.h"
#include "detail/CompletionTokenizer.h"
#include "detail/FieldResolver.h"
#include "detail/Lexical.h"
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/FieldCatalog.h>
#include <ao/utility/String.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
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

    bool startsWithInsensitive(std::string_view candidate, std::string_view prefix)
    {
      return std::ranges::starts_with(candidate, prefix, {}, utility::toAsciiLower, utility::toAsciiLower);
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

    bool hasExactAlias(QueryVariableDescriptor const& descriptor, std::string_view prefix)
    {
      return std::ranges::any_of(descriptor.aliases,
                                 [prefix](std::string_view alias)
                                 { return utility::isEqualIgnoringAsciiCase(alias, prefix); });
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
      auto iter = std::ranges::lower_bound(tokens, end, {}, &detail::CompletionToken::end);

      if (iter == tokens.end() || iter->end != end)
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

    std::size_t skipWhitespaceBefore(std::span<detail::CompletionToken const> tokens, std::size_t cursorPosition)
    {
      auto const* token = tokenEndingAt(tokens, cursorPosition);
      return token != nullptr && token->kind == detail::CompletionTokenKind::Whitespace ? token->begin : cursorPosition;
    }

    bool isSimpleVariableCompletionText(std::string_view text)
    {
      return !text.empty() && detail::isVariableSigil(text.front()) &&
             std::ranges::all_of(text.substr(1), detail::isQueryIdentifierChar);
    }

    bool hasAdjacentIdentifierBefore(std::span<detail::CompletionToken const> tokens, detail::CompletionToken token)
    {
      auto const* previous = tokenEndingAt(tokens, token.begin);

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

    std::optional<QueryVariableCompletion> variableCompletionAtCursor(std::string_view text,
                                                                      std::span<detail::CompletionToken const> tokens,
                                                                      std::size_t cursor)
    {
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

      return QueryVariableCompletion{
        .type = variableTypeForTrigger(value.front()),
        .replacement = {.replaceBegin = token->begin, .replaceEnd = cursor, .prefix = std::string{value.substr(1)}},
      };
    }

    std::optional<Field> fieldEndingAt(std::string_view text,
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

      auto const type = variableTypeForTrigger(value.front());
      auto const name = isSimpleVariableCompletionText(value) ? value.substr(1) : std::string_view{};
      return detail::lookupVariableField(type, name);
    }

    std::optional<detail::CompletionToken> findLastUnclosedListOpen(std::span<detail::CompletionToken const> tokens,
                                                                    std::size_t end)
    {
      std::size_t closed = 0;

      for (auto const token : std::views::reverse(tokens))
      {
        if (token.end > end)
        {
          continue;
        }

        if (token.kind == detail::CompletionTokenKind::CloseList)
        {
          ++closed;
        }
        else if (token.kind == detail::CompletionTokenKind::OpenList)
        {
          if (closed == 0)
          {
            return token;
          }

          --closed;
        }
      }

      return std::nullopt;
    }

    std::size_t findValuePrefixStart(std::span<detail::CompletionToken const> tokens, std::size_t cursor)
    {
      auto start = cursor;

      for (;;)
      {
        auto const* token = tokenEndingAt(tokens, start);

        if (token == nullptr || isValuePrefixBoundary(token->kind))
        {
          break;
        }

        start = token->begin;
      }

      return start;
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

    std::optional<QueryValueCompletion> analyzeValueCompletion(std::string_view text,
                                                               std::span<detail::CompletionToken const> tokens,
                                                               std::size_t cursor)
    {
      auto const valueStart = findValuePrefixStart(tokens, cursor);
      auto operatorEnd = skipWhitespaceBefore(tokens, valueStart);
      auto const* previous = tokenEndingAt(tokens, operatorEnd);
      auto const inList = previous != nullptr && (previous->kind == detail::CompletionTokenKind::OpenList ||
                                                  previous->kind == detail::CompletionTokenKind::Comma);

      if (inList)
      {
        auto optListOpen = findLastUnclosedListOpen(tokens, operatorEnd);

        if (!optListOpen)
        {
          return std::nullopt;
        }

        operatorEnd = skipWhitespaceBefore(tokens, optListOpen->begin);
      }

      auto const* op = tokenEndingAt(tokens, operatorEnd);

      if (op == nullptr || op->kind != detail::CompletionTokenKind::RelationalOperator)
      {
        return std::nullopt;
      }

      auto const spelling = detail::tokenText(text, *op);
      auto const membership = utility::isEqualIgnoringAsciiCase(spelling, "in");

      if ((inList && !membership) || (op->end == cursor && (spelling == "<" || spelling == ">" || membership)))
      {
        return std::nullopt;
      }

      auto optField = fieldEndingAt(text, tokens, skipWhitespaceBefore(tokens, op->begin));

      if (!optField)
      {
        return std::nullopt;
      }

      return QueryValueCompletion{
        .field = *optField,
        .replacement =
          QueryCompletionReplacement{
            .replaceBegin = valueStart,
            .replaceEnd = cursor,
            .prefix = std::string{text.substr(valueStart, cursor - valueStart)},
          },
      };
    }

    std::optional<QueryOperatorCompletion> analyzeOperatorCompletion(std::string_view text,
                                                                     std::span<detail::CompletionToken const> tokens,
                                                                     std::size_t cursor)
    {
      auto prefixStart = cursor;
      auto lvalueEnd = skipWhitespaceBefore(tokens, prefixStart);

      if (auto const* token = tokenEndingAt(tokens, cursor);
          token != nullptr && isOperatorCompletionPrefixToken(text, *token))
      {
        prefixStart = token->begin;
        lvalueEnd = skipWhitespaceBefore(tokens, prefixStart);
      }

      auto optField = fieldEndingAt(text, tokens, lvalueEnd);

      if (!optField)
      {
        return std::nullopt;
      }

      return QueryOperatorCompletion{
        .field = *optField,
        .replacement =
          QueryCompletionReplacement{
            .replaceBegin = lvalueEnd,
            .replaceEnd = cursor,
            .prefix = std::string{text.substr(prefixStart, cursor - prefixStart)},
          },
      };
    }

    std::optional<QueryLogicalOperatorCompletion> analyzeLogicalOperatorCompletion(
      std::string_view text,
      std::span<detail::CompletionToken const> tokens,
      std::size_t cursor)
    {
      auto prefixStart = cursor;
      auto expressionEnd = skipWhitespaceBefore(tokens, prefixStart);

      if (auto const* token = tokenEndingAt(tokens, cursor);
          token != nullptr && isLogicalOperatorCompletionPrefixToken(*token))
      {
        prefixStart = token->begin;
        expressionEnd = skipWhitespaceBefore(tokens, prefixStart);
      }

      auto const pastExpression = std::ranges::upper_bound(tokens, expressionEnd, {}, &detail::CompletionToken::end);

      if (!detail::hasCompletionPredicateShape(
            text.substr(0, expressionEnd), std::span{tokens.begin(), pastExpression}))
      {
        return std::nullopt;
      }

      return QueryLogicalOperatorCompletion{
        .replacement =
          QueryCompletionReplacement{
            .replaceBegin = expressionEnd,
            .replaceEnd = cursor,
            .prefix = std::string{text.substr(prefixStart, cursor - prefixStart)},
          },
      };
    }
  } // namespace

  std::optional<QueryCompletionAnalysis> analyzeQueryCompletion(std::string_view text, std::size_t cursor)
  {
    if (cursor > text.size())
    {
      return std::nullopt;
    }

    auto const tokens = detail::tokenizeCompletionQuery(text);
    auto prefixTokenStorage = std::vector<detail::CompletionToken>{};
    auto prefixTokens = std::span<detail::CompletionToken const>{tokens};

    if (cursor != text.size())
    {
      prefixTokenStorage = detail::tokenizeCompletionQuery(text.substr(0, cursor));
      prefixTokens = prefixTokenStorage;
    }

    if (shouldBlockCompletionAtCursor(tokens, cursor) || hasBlockingPartialTailAtCursor(text, prefixTokens, cursor))
    {
      return std::nullopt;
    }

    if (auto optToken = variableCompletionAtCursor(text, prefixTokens, cursor); optToken)
    {
      return QueryCompletionAnalysis{*optToken};
    }

    if (auto optValueContext = analyzeValueCompletion(text, prefixTokens, cursor); optValueContext)
    {
      return QueryCompletionAnalysis{*optValueContext};
    }

    if (auto optLogicalOperatorContext = analyzeLogicalOperatorCompletion(text, prefixTokens, cursor);
        optLogicalOperatorContext)
    {
      return QueryCompletionAnalysis{*optLogicalOperatorContext};
    }

    if (auto optOperatorContext = analyzeOperatorCompletion(text, prefixTokens, cursor); optOperatorContext)
    {
      return QueryCompletionAnalysis{*optOperatorContext};
    }

    return std::nullopt;
  }

  std::vector<QueryVariableCompletionMatch> completeQueryVariable(VariableType type, std::string_view prefix)
  {
    auto matches = std::vector<QueryVariableCompletionMatch>{};
    auto const descriptors = queryVariableDescriptors(type);

    for (auto const& descriptor : descriptors)
    {
      if (hasExactAlias(descriptor, prefix))
      {
        matches.push_back(QueryVariableCompletionMatch{
          .type = descriptor.type,
          .field = descriptor.field,
          .canonicalName = descriptor.canonicalName,
          .kind = QueryVariableCompletionMatchKind::ExactAlias,
        });
      }
    }

    for (auto const& descriptor : descriptors)
    {
      if (startsWithInsensitive(descriptor.canonicalName, prefix) && !hasExactAlias(descriptor, prefix))
      {
        matches.push_back(QueryVariableCompletionMatch{
          .type = descriptor.type,
          .field = descriptor.field,
          .canonicalName = descriptor.canonicalName,
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
    else if (isStringField(field) || isDictionaryField(field) || isCreditField(field))
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
        case Field::RecordingDate:
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
