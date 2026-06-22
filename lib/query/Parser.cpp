// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#include "detail/Lexical.h"
#include <ao/Error.h>
#include <ao/query/Expression.h>
#include <ao/query/Parser.h>

#include <lexy/action/match.hpp>
#include <lexy/action/parse.hpp>
#include <lexy/callback/adapter.hpp>
#include <lexy/callback/composition.hpp>
#include <lexy/callback/container.hpp>
#include <lexy/callback/forward.hpp>
#include <lexy/callback/noop.hpp>
#include <lexy/dsl/ascii.hpp>
#include <lexy/dsl/brackets.hpp>
#include <lexy/dsl/branch.hpp>
#include <lexy/dsl/choice.hpp>
#include <lexy/dsl/eof.hpp>
#include <lexy/dsl/error.hpp>
#include <lexy/dsl/expression.hpp>
#include <lexy/dsl/list.hpp>
#include <lexy/dsl/literal.hpp>
#include <lexy/dsl/operator.hpp>
#include <lexy/dsl/peek.hpp>
#include <lexy/dsl/production.hpp>
#include <lexy/dsl/punctuator.hpp>
#include <lexy/dsl/separator.hpp>
#include <lexy/dsl/sequence.hpp>
#include <lexy/encoding.hpp>
#include <lexy/input/string_input.hpp>

#include <format>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
  // LEXY grammar: rule/value/op/operand/operation names follow framework conventions.
  namespace dsl = lexy::dsl;
  using namespace ao::query;
  using detail::Constant;
  using detail::Variable;

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
      static constexpr auto op = dsl::op<Operator::Exists>(detail::oplit::kExists);
      using operand = dsl::atom;
    };

    struct MathNot : dsl::prefix_op
    {
      static constexpr auto op =
        dsl::op<Operator::Not>(detail::oplit::kNotWord) / dsl::op<Operator::Not>(detail::oplit::kNotSymbol);
      using operand = MathExists;
    };

    struct MathAdd : dsl::infix_op_right
    {
      static constexpr auto op = dsl::op<Operator::Add>(detail::oplit::kAdd);
      using operand = MathNot;
    };

    struct MathRelational : dsl::infix_op_right
    {
      static constexpr auto op =
        dsl::op<Operator::Equal>(detail::oplit::kEqual) / dsl::op<Operator::NotEqual>(detail::oplit::kNotEqual) /
        dsl::op<Operator::LessEqual>(detail::oplit::kLessEqual) /
        dsl::op<Operator::GreaterEqual>(detail::oplit::kGreaterEqual) / dsl::op<Operator::Like>(detail::oplit::kLike) /
        dsl::op<Operator::In>(detail::oplit::kIn) / dsl::op<Operator::Less>(detail::oplit::kLess) /
        dsl::op<Operator::Greater>(detail::oplit::kGreater);
      using operand = MathAdd;
    };

    struct MathAnd : dsl::infix_op_right
    {
      static constexpr auto op =
        dsl::op<Operator::And>(detail::oplit::kAndWord) / dsl::op<Operator::And>(detail::oplit::kAndSymbol);
      using operand = MathRelational;
    };

    struct MathOr : dsl::infix_op_right
    {
      static constexpr auto op =
        dsl::op<Operator::Or>(detail::oplit::kOrWord) / dsl::op<Operator::Or>(detail::oplit::kOrSymbol);
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
  bool matchesExpressionSyntax(std::string_view expr)
  {
    auto const input = lexy::string_input<lexy::utf8_char_encoding>{expr};
    return lexy::match<Stmt>(input);
  }

  Result<Expression> parse(std::string_view expr)
  {
    auto const input = lexy::string_input<lexy::utf8_char_encoding>{expr};

    if (auto optResult = lexy::parse<Stmt>(input, lexy::noop); optResult)
    {
      auto root = Expression{std::move(optResult).value()};

      normalize(root);

      return root;
    }

    return makeError(Error::Code::FormatRejected, std::format("failed to parse query expression '{}'", expr));
  }
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
