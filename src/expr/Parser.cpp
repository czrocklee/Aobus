// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif

#include <rs/Exception.h>
#include <rs/expr/Parser.h>
#include <rs/expr/Serializer.h>

#include <lexy/dsl.hpp>
#include <lexy/callback.hpp>
#include <lexy/action/parse.hpp>
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

  constexpr auto var_types = lexy::symbol_table<VariableType>
    .map<LEXY_SYMBOL("$")>(VariableType::Metadata)
    .map<LEXY_SYMBOL("@")>(VariableType::Property)
    .map<LEXY_SYMBOL("#")>(VariableType::Tag)
    .map<LEXY_SYMBOL("%")>(VariableType::Custom);

  constexpr auto bool_table = lexy::symbol_table<bool>
    .map<LEXY_SYMBOL("true")>(true)
    .map<LEXY_SYMBOL("false")>(false);

  constexpr auto bareword_identifier = [] {
    auto id = dsl::identifier(dsl::ascii::alpha_digit_underscore);
    return id.reserve(LEXY_KEYWORD("and", id),
                      LEXY_KEYWORD("or", id),
                      LEXY_KEYWORD("not", id));
  }();

  struct variable : lexy::token_production {
      static constexpr auto rule = dsl::symbol<var_types> >> dsl::identifier(dsl::ascii::alpha_underscore, dsl::ascii::alpha_digit_underscore);
      static constexpr auto value = lexy::callback<VariableExpression>(
          [](VariableType type, auto lexeme) { return VariableExpression{type, std::string{lexeme.begin(), lexeme.end()}}; }  // NOLINT(readability-named-parameter)
      );
  };

  struct boolean_constant : lexy::token_production {
      static constexpr auto rule = dsl::symbol<bool_table>(dsl::identifier(dsl::ascii::alpha));
      static constexpr auto value = lexy::forward<bool>;
  };

  struct string_constant : lexy::token_production {
      static constexpr auto rule = 
            (dsl::lit_c<'\''> >> dsl::capture(dsl::until(dsl::lit_c<'\''>)))
          | (dsl::lit_c<'"'>  >> dsl::capture(dsl::until(dsl::lit_c<'"'>)))
          | bareword_identifier;

      static constexpr auto value = lexy::callback<std::string>(
          [](auto lexeme) {  // NOLINT(readability-named-parameter)
              std::string str(lexeme.begin(), lexeme.end());
              if (!str.empty() && (str.back() == '\'' || str.back() == '"')) {
                  str.pop_back();
              }
              return str;
          }
      );
  };

  struct negative_integer : lexy::token_production {
      static constexpr auto rule = dsl::lit_c<'-'> >> dsl::integer<std::int64_t>;
      static constexpr auto value = lexy::callback<std::int64_t>([](std::int64_t val) { return -val; });  // NOLINT(readability-named-parameter)
  };

  struct positive_integer : lexy::token_production {
      static constexpr auto rule = dsl::integer<std::int64_t>;
      static constexpr auto value = lexy::forward<std::int64_t>;
  };

  struct integer_constant {
      static constexpr auto rule = dsl::p<negative_integer> | dsl::p<positive_integer>;
      static constexpr auto value = lexy::forward<std::int64_t>;
  };

  struct constant {
      static constexpr auto rule = dsl::p<boolean_constant> | dsl::p<integer_constant> | dsl::p<string_constant>;
      static constexpr auto value = lexy::construct<ConstantExpression>;
  };

  struct expr : lexy::expression_production {
      struct expected_operand { static constexpr auto name = "expected operand"; };

      struct expr_atom {
          static constexpr auto rule = dsl::list(dsl::parenthesized(dsl::p<expr>) | dsl::p<variable> | dsl::p<constant>);
          static constexpr auto value = lexy::as_list<std::vector<Expression>> >> lexy::callback<Expression>(
              [](std::vector<Expression> list) {
                  Expression result = std::move(list.back());
                  for (auto it = list.rbegin() + 1; it != list.rend(); ++it) {
                      auto bin = std::make_unique<BinaryExpression>();
                      bin->operand = std::move(*it);
                      bin->operation = BinaryExpression::Operation{Operator::Add, std::move(result)};
                      result = Expression{std::move(bin)};
                  }
                  return result;
              }
          );
      };

      static constexpr auto atom = dsl::p<expr_atom> | dsl::error<expected_operand>;

      struct math_not : dsl::prefix_op {
          static constexpr auto op = dsl::op<Operator::Not>(LEXY_KEYWORD("not", dsl::identifier(dsl::ascii::alpha)))
                                   / dsl::op<Operator::Not>(dsl::lit_c<'!'>);
          using operand = dsl::atom;
      };

      struct math_add : dsl::infix_op_right {
          static constexpr auto op = dsl::op<Operator::Add>(dsl::lit_c<'+'>);
          using operand = math_not;
      };

      struct math_relational : dsl::infix_op_right {
          static constexpr auto op = dsl::op<Operator::Equal>(dsl::lit_c<'='>)
                                   / dsl::op<Operator::NotEqual>(LEXY_LIT("!="))
                                   / dsl::op<Operator::LessEqual>(LEXY_LIT("<="))
                                   / dsl::op<Operator::GreaterEqual>(LEXY_LIT(">="))
                                   / dsl::op<Operator::Like>(dsl::lit_c<'~'>)
                                   / dsl::op<Operator::Less>(dsl::lit_c<'<'>)
                                   / dsl::op<Operator::Greater>(dsl::lit_c<'>'>);
          using operand = math_add;
      };

      struct math_and : dsl::infix_op_right {
          static constexpr auto op = dsl::op<Operator::And>(LEXY_KEYWORD("and", dsl::identifier(dsl::ascii::alpha)))
                                   / dsl::op<Operator::And>(LEXY_LIT("&&"));
          using operand = math_relational;
      };

      struct math_or : dsl::infix_op_right {
          static constexpr auto op = dsl::op<Operator::Or>(LEXY_KEYWORD("or", dsl::identifier(dsl::ascii::alpha)))
                                   / dsl::op<Operator::Or>(LEXY_LIT("||"));
          using operand = math_and;
      };

      using operation = math_or;

      static constexpr auto value = lexy::callback<Expression>(
          [](Expression lhs) { return lhs; },
          [](Operator op, Expression expr) {
              auto un = std::make_unique<UnaryExpression>();
              un->op = op;
              un->operand = std::move(expr);
              return Expression{std::move(un)};
          },
          [](Expression lhs, Operator op, Expression rhs) {
              auto bin = std::make_unique<BinaryExpression>();
              bin->operand = std::move(lhs);
              bin->operation = BinaryExpression::Operation{op, std::move(rhs)};
              return Expression{std::move(bin)};
          }
      );
  };

  struct stmt {
      static constexpr auto whitespace = dsl::ascii::space;
      static constexpr auto rule = dsl::p<expr> + dsl::eof;
      static constexpr auto value = lexy::forward<Expression>;
  };
}

namespace rs::expr
{
  Expression parse(std::string_view expr)
  {
    auto input = lexy::string_input<lexy::utf8_char_encoding>(expr);
    auto result = lexy::parse<stmt>(input, lexy::noop);
    
    if (result.has_value()) {
        Expression root = std::move(result).value();
        normalize(root);
        return root;
    }
    RS_THROW_FORMAT(rs::Exception, "parsing {} error", expr);
  }
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
