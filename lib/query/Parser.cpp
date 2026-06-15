// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#include <ao/Exception.h>
#include <ao/query/Expression.h>
#include <ao/query/Parser.h>

#include <lexy/action/parse.hpp>
#include <lexy/callback/adapter.hpp>
#include <lexy/callback/composition.hpp>
#include <lexy/callback/container.hpp>
#include <lexy/callback/forward.hpp>
#include <lexy/callback/noop.hpp>
#include <lexy/callback/object.hpp>
#include <lexy/callback/string.hpp>
#include <lexy/dsl/ascii.hpp>
#include <lexy/dsl/brackets.hpp>
#include <lexy/dsl/branch.hpp>
#include <lexy/dsl/capture.hpp>
#include <lexy/dsl/choice.hpp>
#include <lexy/dsl/delimited.hpp>
#include <lexy/dsl/digit.hpp>
#include <lexy/dsl/eof.hpp>
#include <lexy/dsl/error.hpp>
#include <lexy/dsl/expression.hpp>
#include <lexy/dsl/identifier.hpp>
#include <lexy/dsl/if.hpp>
#include <lexy/dsl/integer.hpp>
#include <lexy/dsl/list.hpp>
#include <lexy/dsl/literal.hpp>
#include <lexy/dsl/operator.hpp>
#include <lexy/dsl/option.hpp>
#include <lexy/dsl/peek.hpp>
#include <lexy/dsl/production.hpp>
#include <lexy/dsl/punctuator.hpp>
#include <lexy/dsl/separator.hpp>
#include <lexy/dsl/sequence.hpp>
#include <lexy/dsl/sign.hpp>
#include <lexy/dsl/symbol.hpp>
#include <lexy/dsl/token.hpp>
#include <lexy/dsl/unicode.hpp>
#include <lexy/encoding.hpp>
#include <lexy/grammar.hpp>
#include <lexy/input/string_input.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
  // LEXY grammar: rule/value/op/operand/operation names follow framework conventions.
  namespace dsl = lexy::dsl;
  using namespace ao::query;

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

  struct StringConstant : lexy::token_production
  {
    static constexpr auto rule = dsl::single_quoted(-dsl::unicode::control, kStringEscape) |
                                 dsl::quoted(-dsl::unicode::control, kStringEscape) | kBarewordIdentifier;

    static constexpr auto value = lexy::as_string<std::string>;
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

  struct ConstantList final
  {
    static constexpr auto rule = dsl::square_bracketed.list(dsl::p<Constant>, dsl::sep(dsl::comma));
    static constexpr auto value = lexy::as_list<std::vector<ConstantExpression>> >>
                                  lexy::callback<ListExpression>(
                                    [](std::vector<ConstantExpression> values)
                                    { return ListExpression{.values = std::move(values)}; });
  };

  struct ConstantRange final
  {
    static constexpr auto rule =
      dsl::peek(dsl::p<Constant> + LEXY_LIT("..")) >> (dsl::p<Constant> + LEXY_LIT("..") + dsl::p<Constant>);
    static constexpr auto value = lexy::callback<RangeExpression>(
      [](ConstantExpression lower, ConstantExpression upper)
      { return RangeExpression{.lower = std::move(lower), .upper = std::move(upper)}; });
  };

  struct Expr : lexy::expression_production
  {
    struct ExpectedOperand
    {
      static constexpr auto name = "expected operand";
    };

    struct ExprAtom
    {
      static constexpr auto rule = dsl::list(dsl::parenthesized(dsl::p<Expr>) | dsl::p<Variable> |
                                             dsl::p<ConstantList> | dsl::p<ConstantRange> | dsl::p<Constant>);
      static constexpr auto value = lexy::as_list<std::vector<Expression>> >>
                                    lexy::callback<Expression>(
                                      [](std::vector<Expression> list)
                                      {
                                        if (list.empty())
                                        {
                                          return Expression{};
                                        }

                                        auto result = Expression{std::move(list.back())};

                                        for (auto& item : list | std::views::reverse | std::views::drop(1))
                                        {
                                          auto binPtr = std::make_unique<BinaryExpression>();
                                          binPtr->operand = std::move(item);
                                          binPtr->optOperation = BinaryExpression::Operation{
                                            .op = Operator::Add, .operand = std::move(result)};
                                          result = Expression{std::move(binPtr)};
                                        }

                                        return result;
                                      });
    };

    static constexpr auto atom = dsl::p<ExprAtom> | dsl::error<ExpectedOperand>;

    struct MathExists : dsl::postfix_op
    {
      static constexpr auto op = dsl::op<Operator::Exists>(dsl::lit_c<'?'>);
      using operand = dsl::atom;
    };

    struct MathNot : dsl::prefix_op
    {
      static constexpr auto op = dsl::op<Operator::Not>(LEXY_KEYWORD("not", dsl::identifier(dsl::ascii::alpha))) /
                                 dsl::op<Operator::Not>(dsl::lit_c<'!'>);
      using operand = MathExists;
    };

    struct MathAdd : dsl::infix_op_right
    {
      static constexpr auto op = dsl::op<Operator::Add>(dsl::lit_c<'+'>);
      using operand = MathNot;
    };

    struct MathRelational : dsl::infix_op_right
    {
      static constexpr auto op =
        dsl::op<Operator::Equal>(dsl::lit_c<'='>) / dsl::op<Operator::NotEqual>(LEXY_LIT("!=")) /
        dsl::op<Operator::LessEqual>(LEXY_LIT("<=")) / dsl::op<Operator::GreaterEqual>(LEXY_LIT(">=")) /
        dsl::op<Operator::Like>(dsl::lit_c<'~'>) / dsl::op<Operator::In>(LEXY_KEYWORD("in", kBarewordIdentifier)) /
        dsl::op<Operator::Less>(dsl::lit_c<'<'>) / dsl::op<Operator::Greater>(dsl::lit_c<'>'>);
      using operand = MathAdd;
    };

    struct MathAnd : dsl::infix_op_right
    {
      static constexpr auto op = dsl::op<Operator::And>(LEXY_KEYWORD("and", dsl::identifier(dsl::ascii::alpha))) /
                                 dsl::op<Operator::And>(LEXY_LIT("&&"));
      using operand = MathRelational;
    };

    struct MathOr : dsl::infix_op_right
    {
      static constexpr auto op = dsl::op<Operator::Or>(LEXY_KEYWORD("or", dsl::identifier(dsl::ascii::alpha))) /
                                 dsl::op<Operator::Or>(LEXY_LIT("||"));
      using operand = MathAnd;
    };

    using operation = MathOr;

    static constexpr auto value = lexy::callback<Expression>([](Expression lhs) { return lhs; },
                                                             [](Operator op, Expression expr)
                                                             {
                                                               auto unPtr = std::make_unique<UnaryExpression>();
                                                               unPtr->op = op;
                                                               unPtr->operand = std::move(expr);
                                                               return Expression{std::move(unPtr)};
                                                             },
                                                             [](Expression expr, Operator op)
                                                             {
                                                               auto unPtr = std::make_unique<UnaryExpression>();
                                                               unPtr->op = op;
                                                               unPtr->operand = std::move(expr);
                                                               return Expression{std::move(unPtr)};
                                                             },
                                                             [](Expression lhs, Operator op, Expression rhs)
                                                             {
                                                               auto binPtr = std::make_unique<BinaryExpression>();
                                                               binPtr->operand = std::move(lhs);
                                                               binPtr->optOperation = BinaryExpression::Operation{
                                                                 .op = op, .operand = std::move(rhs)};
                                                               return Expression{std::move(binPtr)};
                                                             });
  };

  struct Stmt final
  {
    static constexpr auto whitespace = dsl::ascii::space;
    static constexpr auto rule = dsl::p<Expr> + dsl::eof;
    static constexpr auto value = lexy::forward<Expression>;
  };
}

namespace ao::query
{
  Expression parse(std::string_view expr)
  {
    auto const input = lexy::string_input<lexy::utf8_char_encoding>{expr}; // NOLINT(aobus-modernize-use-ctad)

    if (auto optResult = lexy::parse<Stmt>(input, lexy::noop); optResult)
    {
      auto root = Expression{std::move(optResult).value()};

      normalize(root);

      return root;
    }

    throwException<Exception>("failed to parse query expression '{}'", expr);
  }
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
