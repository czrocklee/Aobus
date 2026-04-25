// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif

#include <rs/Exception.h>
#include <rs/expr/Parser.h>
#include <rs/expr/Serializer.h>

#include <lexy/action/parse.hpp>
#include <lexy/callback.hpp>
#include <lexy/dsl.hpp>
#include <lexy/input/string_input.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
  namespace dsl = lexy::dsl;
  using namespace rs::expr;

  constexpr auto kVarTypes = lexy::symbol_table<VariableType>
    .map<LEXY_SYMBOL("$")>(VariableType::Metadata)
    .map<LEXY_SYMBOL("@")>(VariableType::Property)
    .map<LEXY_SYMBOL("#")>(VariableType::Tag)
    .map<LEXY_SYMBOL("%")>(VariableType::Custom);

  constexpr auto kBoolTable = lexy::symbol_table<bool>.map<LEXY_SYMBOL("true")>(true).map<LEXY_SYMBOL("false")>(false);

  constexpr auto kBarewordIdentifier = []
  {
    auto id = dsl::identifier(dsl::ascii::alpha_digit_underscore);
    return id.reserve(LEXY_KEYWORD("and", id), LEXY_KEYWORD("or", id), LEXY_KEYWORD("not", id));
  }();

  struct Variable : lexy::token_production
  {
    static constexpr auto rule = dsl::symbol<kVarTypes> >>
                                 dsl::identifier(dsl::ascii::alpha_underscore, dsl::ascii::alpha_digit_underscore);
    static constexpr auto value = lexy::callback<VariableExpression>(
      [](VariableType type, auto lexeme)
      {
        return VariableExpression{type, std::string{lexeme.begin(), lexeme.end()}};
      } // NOLINT(readability-named-parameter)
    );
  };

  struct BooleanConstant : lexy::token_production
  {
    static constexpr auto rule = dsl::symbol<kBoolTable>(dsl::identifier(dsl::ascii::alpha));
    static constexpr auto value = lexy::forward<bool>;
  };

  struct StringConstant : lexy::token_production
  {
    static constexpr auto rule = (dsl::lit_c<'\''> >> dsl::capture(dsl::until(dsl::lit_c<'\''>))) |
                                 (dsl::lit_c<'"'> >> dsl::capture(dsl::until(dsl::lit_c<'"'>))) | kBarewordIdentifier;

    static constexpr auto value = lexy::callback<std::string>(
          [](auto lexeme) {  // NOLINT(readability-named-parameter)
              auto str = std::string{lexeme.begin(), lexeme.end()};
              
              if (!str.empty() && (str.back() == '\'' || str.back() == '"')) {
                  str.pop_back();
              }
              
              return str;
          }
      );
  };

  struct NegativeInteger : lexy::token_production
  {
    static constexpr auto rule = dsl::lit_c<'-'> >> dsl::integer<std::int64_t>;
    static constexpr auto value =
      lexy::callback<std::int64_t>([](std::int64_t val) { return -val; }); // NOLINT(readability-named-parameter)
  };

  struct PositiveInteger : lexy::token_production
  {
    static constexpr auto rule = dsl::integer<std::int64_t>;
    static constexpr auto value = lexy::forward<std::int64_t>;
  };

  struct IntegerConstant
  {
    static constexpr auto rule = dsl::p<NegativeInteger> | dsl::p<PositiveInteger>;
    static constexpr auto value = lexy::forward<std::int64_t>;
  };

  struct UnitConstant : lexy::token_production
  {
    static constexpr auto kUnitToken =
      dsl::token(dsl::minus_sign + dsl::digits<> + dsl::opt(dsl::lit_c<'.'> >> dsl::digits<>) +
                 dsl::identifier(dsl::ascii::alpha).pattern());
    static constexpr auto rule = dsl::peek(kUnitToken) >> dsl::capture(kUnitToken);
    static constexpr auto value = lexy::callback<UnitConstantExpression>(
      [](auto lexeme) { return UnitConstantExpression{std::string{lexeme.begin(), lexeme.end()}}; });
  };

  struct Constant
  {
    static constexpr auto rule =
      dsl::p<BooleanConstant> | dsl::p<UnitConstant> | dsl::p<IntegerConstant> | dsl::p<StringConstant>;
    static constexpr auto value = lexy::construct<ConstantExpression>;
  };

  struct Expr : lexy::expression_production
  {
    struct ExpectedOperand
    {
      static constexpr auto name = "expected operand";
    };

    struct ExprAtom
    {
      static constexpr auto rule = dsl::list(dsl::parenthesized(dsl::p<Expr>) | dsl::p<Variable> | dsl::p<Constant>);
      static constexpr auto value = lexy::as_list<std::vector<Expression>> >>
                                    lexy::callback<Expression>(
                                      [](std::vector<Expression> list)
                                      {
                                        auto result = Expression{std::move(list.back())};
                                        
                                        for (auto it = list.rbegin() + 1; it != list.rend(); ++it)
                                        {
                                          auto bin = std::make_unique<BinaryExpression>();
                                          bin->operand = std::move(*it);
                                          bin->operation =
                                            BinaryExpression::Operation{Operator::Add, std::move(result)};
                                          result = Expression{std::move(bin)};
                                        }
                                        
                                        return result;
                                      });
    };

    static constexpr auto atom = dsl::p<ExprAtom> | dsl::error<ExpectedOperand>;

    struct MathNot : dsl::prefix_op
    {
      static constexpr auto op = dsl::op<Operator::Not>(LEXY_KEYWORD("not", dsl::identifier(dsl::ascii::alpha))) /
                                 dsl::op<Operator::Not>(dsl::lit_c<'!'>);
      using operand = dsl::atom;
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
        dsl::op<Operator::Like>(dsl::lit_c<'~'>) / dsl::op<Operator::Less>(dsl::lit_c<'<'>) /
        dsl::op<Operator::Greater>(dsl::lit_c<'>'>);
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
                                                               auto un = std::make_unique<UnaryExpression>();
                                                               un->op = op;
                                                               un->operand = std::move(expr);
                                                               return Expression{std::move(un)};
                                                             },
                                                             [](Expression lhs, Operator op, Expression rhs)
                                                             {
                                                               auto bin = std::make_unique<BinaryExpression>();
                                                               bin->operand = std::move(lhs);
                                                               bin->operation =
                                                                 BinaryExpression::Operation{op, std::move(rhs)};
                                                               return Expression{std::move(bin)};
                                                             });
  };

  struct Stmt
  {
    static constexpr auto whitespace = dsl::ascii::space;
    static constexpr auto rule = dsl::p<Expr> + dsl::eof;
    static constexpr auto value = lexy::forward<Expression>;
  };
}

namespace rs::expr
{
  Expression parse(std::string_view expr)
  {
    auto input = lexy::string_input<lexy::utf8_char_encoding>{expr};
    auto result = lexy::parse<Stmt>(input, lexy::noop);

    if (result.has_value())
    {
      auto root = Expression{std::move(result).value()};
      normalize(root);
      return root;
    }
    
    RS_THROW_FORMAT(rs::Exception, "parsing {} error", expr);
  }
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
