// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "CompletionTokenizer.h"

#include "CompletionTokenRules.h"
#include "Lexical.h"

#include <lexy/action/scan.hpp>
#include <lexy/callback/noop.hpp>
#include <lexy/dsl/ascii.hpp>
#include <lexy/dsl/capture.hpp>
#include <lexy/dsl/code_point.hpp>
#include <lexy/dsl/literal.hpp>
#include <lexy/dsl/production.hpp>
#include <lexy/dsl/scan.hpp>
#include <lexy/encoding.hpp>
#include <lexy/input/string_input.hpp>
#include <lexy/lexeme.hpp>

#include <cassert>
#include <cstddef>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ao::query::detail
{
  namespace
  {
    constexpr bool isPartialTailStart(char ch)
    {
      return isVariableSigil(ch) || ch == '"' || ch == '\'';
    }

    constexpr bool isStringEscapeChar(char ch)
    {
      // Keep in sync with detail::Lexical.h's kStringEscapeSymbols.
      return ch == '"' || ch == '\\' || ch == '\'' || ch == 'n' || ch == 't' || ch == 'r';
    }

    std::size_t findClosingQuote(std::string_view text, std::size_t quoteBegin)
    {
      auto const quote = text[quoteBegin];

      for (auto offset = quoteBegin + 1; offset < text.size(); ++offset)
      {
        if (auto const ch = text[offset]; ch == '\\')
        {
          // An invalid or dangling escape makes the whole quoted literal unterminated,
          // so treat it as a PartialTail and do not let a later quote close it.
          if (offset + 1 < text.size() && isStringEscapeChar(text[offset + 1]))
          {
            ++offset;
          }
          else
          {
            return std::string_view::npos;
          }
        }
        else if (ch == quote)
        {
          return offset;
        }
      }

      return std::string_view::npos;
    }

    bool isPartialTailAt(std::string_view text, std::size_t begin)
    {
      if (begin >= text.size() || !isPartialTailStart(text[begin]))
      {
        return false;
      }

      if (text[begin] == '"' || text[begin] == '\'')
      {
        return findClosingQuote(text, begin) == std::string_view::npos;
      }

      if (isSystemVariableSigil(text[begin]))
      {
        return text.size() - begin == 1 || !isQueryIdentifierStart(text[begin + 1]);
      }

      if (text.size() - begin == 1)
      {
        return true;
      }

      if (text[begin + 1] == '"')
      {
        return findClosingQuote(text, begin + 1) == std::string_view::npos;
      }

      if (text[begin + 1] == '[')
      {
        if (text.size() - begin <= 2 || text[begin + 2] != '"')
        {
          return true;
        }

        auto const quoteEnd = findClosingQuote(text, begin + 2);

        return quoteEnd == std::string_view::npos || text.size() - quoteEnd <= 1 || text[quoteEnd + 1] != ']';
      }

      return !isQueryIdentifierChar(text[begin + 1]);
    }

    constexpr std::size_t offsetFrom(std::string_view text, char const* address)
    {
      return static_cast<std::size_t>(address - text.data());
    }

    template<typename Scanner, typename Rule>
    bool branchToken(Scanner& scanner,
                     Rule rule,
                     std::string_view text,
                     CompletionTokenKind kind,
                     std::vector<CompletionToken>& tokens)
    {
      using Reader = std::remove_cvref_t<decltype(scanner.remaining_input().reader())>;
      auto result = lexy::scan_result<lexy::lexeme<Reader>>{};

      if (!scanner.branch(result, lexy::dsl::capture(rule)))
      {
        return false;
      }

      if (!result)
      {
        return false;
      }

      tokens.push_back(CompletionToken{
        .kind = kind,
        .begin = offsetFrom(text, result.value().begin()),
        .end = offsetFrom(text, result.value().end()),
      });
      return true;
    }

    template<typename Scanner>
    bool branchWhitespace(Scanner& scanner, std::string_view text, std::vector<CompletionToken>& tokens)
    {
      auto const begin = scanner.position();

      if (!scanner.branch(lexy::dsl::ascii::space))
      {
        return false;
      }

      while (scanner.branch(lexy::dsl::ascii::space))
      {
      }

      tokens.push_back(CompletionToken{
        .kind = CompletionTokenKind::Whitespace,
        .begin = offsetFrom(text, begin),
        .end = offsetFrom(text, scanner.position()),
      });
      return true;
    }

    template<typename Scanner>
    // NOLINTNEXTLINE(readability-function-cognitive-complexity) -- lexy macros expand into synthetic branches.
    bool branchKnownToken(Scanner& scanner, std::string_view text, std::vector<CompletionToken>& tokens)
    {
      namespace dsl = lexy::dsl;

      return branchWhitespace(scanner, text, tokens) ||
             branchToken(scanner, dsl::p<AsToken<SystemVariable>>, text, CompletionTokenKind::Variable, tokens) ||
             branchToken(scanner, dsl::p<AsToken<UserVariable>>, text, CompletionTokenKind::Variable, tokens) ||
             branchToken(
               scanner, dsl::p<AsToken<QuotedStringConstant>>, text, CompletionTokenKind::StringLiteral, tokens) ||
             branchToken(scanner, dsl::p<AsToken<UnitConstant>>, text, CompletionTokenKind::UnitLiteral, tokens) ||
             branchToken(
               scanner, dsl::p<AsToken<BooleanConstant>>, text, CompletionTokenKind::BooleanLiteral, tokens) ||
             branchToken(
               scanner, dsl::p<AsToken<NegativeInteger>>, text, CompletionTokenKind::IntegerLiteral, tokens) ||
             branchToken(
               scanner, dsl::p<AsToken<PositiveInteger>>, text, CompletionTokenKind::IntegerLiteral, tokens) ||
             branchToken(scanner, dsl::p<LogicalOperatorToken>, text, CompletionTokenKind::LogicalOperator, tokens) ||
             branchToken(
               scanner, dsl::p<RelationalOperatorToken>, text, CompletionTokenKind::RelationalOperator, tokens) ||
             branchToken(scanner, dsl::p<PrefixOperatorToken>, text, CompletionTokenKind::PrefixOperator, tokens) ||
             branchToken(scanner, dsl::p<PostfixOperatorToken>, text, CompletionTokenKind::PostfixOperator, tokens) ||
             branchToken(scanner, dsl::p<AddOperatorToken>, text, CompletionTokenKind::AddOperator, tokens) ||
             branchToken(scanner, LEXY_LIT(".."), text, CompletionTokenKind::RangeDelimiter, tokens) ||
             branchToken(scanner, dsl::lit_c<'['>, text, CompletionTokenKind::OpenList, tokens) ||
             branchToken(scanner, dsl::lit_c<']'>, text, CompletionTokenKind::CloseList, tokens) ||
             branchToken(scanner, dsl::lit_c<'('>, text, CompletionTokenKind::OpenGroup, tokens) ||
             branchToken(scanner, dsl::lit_c<')'>, text, CompletionTokenKind::CloseGroup, tokens) ||
             branchToken(scanner, dsl::lit_c<','>, text, CompletionTokenKind::Comma, tokens) ||
             branchToken(scanner, dsl::p<AsToken<BarewordStringConstant>>, text, CompletionTokenKind::Bareword, tokens);
    }
  } // namespace

  std::vector<CompletionToken> tokenizeCompletionQuery(std::string_view text)
  {
    auto input = lexy::string_input<lexy::utf8_char_encoding>{text};
    auto scanner = lexy::scan(input, lexy::noop);
    auto tokens = std::vector<CompletionToken>{};

    while (scanner && !scanner.is_at_eof())
    {
      if (auto const tokenBegin = offsetFrom(text, scanner.position()); isPartialTailAt(text, tokenBegin))
      {
        tokens.push_back(
          CompletionToken{.kind = CompletionTokenKind::PartialTail, .begin = tokenBegin, .end = text.size()});
        break;
      }

      if (branchKnownToken(scanner, text, tokens))
      {
        continue;
      }

      auto const begin = offsetFrom(text, scanner.position());

      if (begin < text.size() && isPartialTailStart(text[begin]))
      {
        tokens.push_back(CompletionToken{.kind = CompletionTokenKind::PartialTail, .begin = begin, .end = text.size()});
        break;
      }

      scanner.parse(lexy::dsl::code_point);
      tokens.push_back(CompletionToken{
        .kind = scanner ? CompletionTokenKind::Unknown : CompletionTokenKind::PartialTail,
        .begin = begin,
        .end = scanner ? offsetFrom(text, scanner.position()) : text.size(),
      });
    }

    return tokens;
  }

  std::string_view tokenText(std::string_view text, CompletionToken token)
  {
    assert(token.begin <= token.end && token.end <= text.size());
    return text.substr(token.begin, token.end - token.begin);
  }
} // namespace ao::query::detail
