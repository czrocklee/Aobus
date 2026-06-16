// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/query/Expression.h>

#include <lexy/callback/adapter.hpp>
#include <lexy/callback/forward.hpp>
#include <lexy/callback/object.hpp>
#include <lexy/callback/string.hpp>
#include <lexy/dsl/ascii.hpp>
#include <lexy/dsl/brackets.hpp>
#include <lexy/dsl/capture.hpp>
#include <lexy/dsl/delimited.hpp>
#include <lexy/dsl/digit.hpp>
#include <lexy/dsl/identifier.hpp>
#include <lexy/dsl/if.hpp>
#include <lexy/dsl/integer.hpp>
#include <lexy/dsl/literal.hpp>
#include <lexy/dsl/option.hpp>
#include <lexy/dsl/peek.hpp>
#include <lexy/dsl/production.hpp>
#include <lexy/dsl/sign.hpp>
#include <lexy/dsl/symbol.hpp>
#include <lexy/dsl/token.hpp>
#include <lexy/dsl/unicode.hpp>
#include <lexy/grammar.hpp>

#include <cstdint>
#include <ranges>
#include <string>
#include <utility>

namespace ao::query::detail
{
  namespace dsl = lexy::dsl;

  // Character classification helpers shared between the parser, the completion tokenizer, and the
  // completion analyzer. Keeping them in one place prevents the tokenizer and analyzer from drifting.
  constexpr bool isAsciiAlpha(char ch)
  {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
  }

  constexpr bool isAsciiDigit(char ch)
  {
    return ch >= '0' && ch <= '9';
  }

  constexpr bool isQueryIdentifierStart(char ch)
  {
    return isAsciiAlpha(ch) || ch == '_';
  }

  constexpr bool isQueryIdentifierChar(char ch)
  {
    return isAsciiAlpha(ch) || isAsciiDigit(ch) || ch == '_';
  }

  constexpr bool isSystemVariableSigil(char ch)
  {
    return ch == '$' || ch == '@';
  }

  constexpr bool isUserVariableSigil(char ch)
  {
    return ch == '#' || ch == '%';
  }

  constexpr bool isVariableSigil(char ch)
  {
    return isSystemVariableSigil(ch) || isUserVariableSigil(ch);
  }

  constexpr auto kSystemVarTypes =
    lexy::symbol_table<VariableType>.map<LEXY_SYMBOL("$")>(VariableType::Metadata).map<LEXY_SYMBOL("@")>(VariableType::Property);

  constexpr auto kUserVarTypes =
    lexy::symbol_table<VariableType>.map<LEXY_SYMBOL("#")>(VariableType::Tag).map<LEXY_SYMBOL("%")>(VariableType::Custom);

  constexpr auto kBoolTable = lexy::symbol_table<bool>.map<LEXY_SYMBOL("true")>(true).map<LEXY_SYMBOL("false")>(false);

  // Shared escape sequences for quoted strings and user-variable names:
  // \", \\, \', \n, \t and \r are recognized in both single- and double-quoted literals.
  constexpr auto kStringEscapeSymbols =
    lexy::symbol_table<char>.map<'"'>('"').map<'\\'>('\\').map<'\''>('\'').map<'n'>('\n').map<'t'>('\t').map<'r'>('\r');

  constexpr auto kStringEscape = dsl::backslash_escape.symbol<kStringEscapeSymbols>(
    dsl::lit_c<'"'> / dsl::lit_c<'\\'> / dsl::lit_c<'\''> / dsl::lit_c<'n'> / dsl::lit_c<'t'> / dsl::lit_c<'r'>);

  constexpr auto kBarewordIdentifier = []
  {
    auto const id = dsl::identifier(dsl::ascii::alpha_digit_underscore);

    return id.reserve(LEXY_KEYWORD("and", id), LEXY_KEYWORD("or", id), LEXY_KEYWORD("not", id), LEXY_KEYWORD("in", id));
  }();

  // Operator spellings, defined once and shared between the parser's expression grammar (dsl::op<>)
  // and the completion tokenizer's operator-token rules, so a new spelling cannot drift between them.
  namespace oplit
  {
    constexpr auto kEqual = dsl::lit_c<'='>;
    constexpr auto kNotEqual = LEXY_LIT("!=");
    constexpr auto kLessEqual = LEXY_LIT("<=");
    constexpr auto kGreaterEqual = LEXY_LIT(">=");
    constexpr auto kLike = dsl::lit_c<'~'>;
    constexpr auto kIn = LEXY_KEYWORD("in", kBarewordIdentifier);
    constexpr auto kLess = dsl::lit_c<'<'>;
    constexpr auto kGreater = dsl::lit_c<'>'>;
    constexpr auto kAndWord = LEXY_KEYWORD("and", dsl::identifier(dsl::ascii::alpha));
    constexpr auto kAndSymbol = LEXY_LIT("&&");
    constexpr auto kOrWord = LEXY_KEYWORD("or", dsl::identifier(dsl::ascii::alpha));
    constexpr auto kOrSymbol = LEXY_LIT("||");
    constexpr auto kNotWord = LEXY_KEYWORD("not", dsl::identifier(dsl::ascii::alpha));
    constexpr auto kNotSymbol = dsl::lit_c<'!'>;
    constexpr auto kExists = dsl::lit_c<'?'>;
    constexpr auto kAdd = dsl::lit_c<'+'>;
  } // namespace oplit

  struct SystemVariable : lexy::token_production
  {
    static constexpr auto rule = dsl::symbol<kSystemVarTypes> >>
                                 dsl::identifier(dsl::ascii::alpha_underscore, dsl::ascii::alpha_digit_underscore);
    static constexpr auto value = lexy::callback<VariableExpression>(
      [](VariableType type, auto lexeme) { return VariableExpression{type, lexeme | std::ranges::to<std::string>()}; });
  };

  struct SimpleUserVariableName : lexy::token_production
  {
    static constexpr auto rule = dsl::identifier(dsl::ascii::alpha_digit_underscore);
    static constexpr auto value =
      lexy::callback<std::string>([](auto lexeme) { return lexeme | std::ranges::to<std::string>(); });
  };

  struct QuotedUserVariableName : lexy::token_production
  {
    static constexpr auto rule = dsl::peek(dsl::lit_c<'"'>) >>
                                 (dsl::peek_not(LEXY_LIT("\"\"")) + dsl::quoted(-dsl::unicode::control, kStringEscape));
    static constexpr auto value = lexy::as_string<std::string>;
  };

  struct BracketedQuotedUserVariableName final
  {
    static constexpr auto rule = dsl::square_bracketed(dsl::p<QuotedUserVariableName>);
    static constexpr auto value = lexy::forward<std::string>;
  };

  struct UserVariableName final
  {
    static constexpr auto rule =
      dsl::p<QuotedUserVariableName> | dsl::p<BracketedQuotedUserVariableName> | dsl::p<SimpleUserVariableName>;
    static constexpr auto value = lexy::forward<std::string>;
  };

  struct UserVariable : lexy::token_production
  {
    static constexpr auto rule = dsl::symbol<kUserVarTypes> >> dsl::p<UserVariableName>;
    static constexpr auto value = lexy::callback<VariableExpression>(
      [](VariableType type, std::string name) { return VariableExpression{.type = type, .name = std::move(name)}; });
  };

  struct Variable final
  {
    static constexpr auto rule = dsl::p<SystemVariable> | dsl::p<UserVariable>;
    static constexpr auto value = lexy::forward<VariableExpression>;
  };

  struct BooleanConstant : lexy::token_production
  {
    static constexpr auto rule = dsl::symbol<kBoolTable>(dsl::identifier(dsl::ascii::alpha));
    static constexpr auto value = lexy::forward<bool>;
  };

  struct BarewordStringConstant : lexy::token_production
  {
    static constexpr auto rule = kBarewordIdentifier;
    static constexpr auto value = lexy::as_string<std::string>;
  };

  struct QuotedStringConstant : lexy::token_production
  {
    static constexpr auto rule =
      dsl::single_quoted(-dsl::unicode::control, kStringEscape) | dsl::quoted(-dsl::unicode::control, kStringEscape);
    static constexpr auto value = lexy::as_string<std::string>;
  };

  struct StringConstant final
  {
    static constexpr auto rule = dsl::p<QuotedStringConstant> | dsl::p<BarewordStringConstant>;
    static constexpr auto value = lexy::forward<std::string>;
  };

  struct NegativeInteger : lexy::token_production
  {
    static constexpr auto rule = dsl::lit_c<'-'> >> dsl::integer<std::int64_t>;
    static constexpr auto value = lexy::callback<std::int64_t>([](std::int64_t val) { return -val; });
  };

  struct PositiveInteger : lexy::token_production
  {
    static constexpr auto rule = dsl::integer<std::int64_t>;
    static constexpr auto value = lexy::forward<std::int64_t>;
  };

  struct IntegerConstant final
  {
    static constexpr auto rule = dsl::p<NegativeInteger> | dsl::p<PositiveInteger>;
    static constexpr auto value = lexy::forward<std::int64_t>;
  };

  struct UnitConstant : lexy::token_production
  {
    static constexpr auto kUnitToken =
      dsl::token(dsl::minus_sign + dsl::digits<> + dsl::opt(dsl::lit_c<'.'> >> dsl::digits<>) +
                 decltype(dsl::identifier(dsl::ascii::alpha, dsl::ascii::alpha_digit))::pattern());
    static constexpr auto rule = dsl::peek(kUnitToken) >> dsl::capture(kUnitToken);
    static constexpr auto value = lexy::callback<UnitConstantExpression>(
      [](auto lexeme) { return UnitConstantExpression{lexeme | std::ranges::to<std::string>()}; });
  };

  struct Constant final
  {
    static constexpr auto rule =
      dsl::p<BooleanConstant> | dsl::p<UnitConstant> | dsl::p<IntegerConstant> | dsl::p<StringConstant>;
    static constexpr auto value = lexy::construct<ConstantExpression>;
  };
} // namespace ao::query::detail
