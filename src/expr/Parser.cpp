// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#define BOOST_SPIRIT_X3_UNICODE

#include <rs/core/Exception.h>
#include <rs/expr/Parser.h>
#include <rs/expr/Serializer.h>

#include <boost/fusion/include/adapt_struct.hpp>
#include <boost/spirit/home/x3.hpp>

BOOST_FUSION_ADAPT_STRUCT(rs::expr::BinaryExpression::Operation, op, operand)
BOOST_FUSION_ADAPT_STRUCT(rs::expr::BinaryExpression, operand, operation)
BOOST_FUSION_ADAPT_STRUCT(rs::expr::UnaryExpression, op, operand)
BOOST_FUSION_ADAPT_STRUCT(rs::expr::VariableExpression, type, name)

namespace
{
  namespace x3 = boost::spirit::x3;
  using namespace rs::expr;

  x3::symbols<VariableType> const varType{{
    {"$", VariableType::Metadata},
    {"@", VariableType::Property},
    {"#", VariableType::Tag},
  }};

  x3::symbols<Operator> const logicalAndOperator{{{"and", Operator::And}, {"&&", Operator::And}}};
  x3::symbols<Operator> const logicalOrOperator{{{"or", Operator::Or}, {"||", Operator::Or}}};
  x3::symbols<Operator> const logicalNotOperator{{{"not", Operator::Not}, {"!", Operator::Not}}};
  x3::symbols<Operator> const arithmeticOperator{{{"+", Operator::Add}}};
  x3::symbols<Operator> const strcatOperator{{{"", Operator::Add}}};
  x3::symbols<Operator> const relationalOperator{{{"=", Operator::Equal},
                                                  {"!=", Operator::NotEqual},
                                                  {"~", Operator::Like},
                                                  {"<", Operator::Less},
                                                  {"<=", Operator::LessEqual},
                                                  {">", Operator::Greater},
                                                  {">=", Operator::GreaterEqual}}};

  auto setupFieldId = [](auto& ctx) {
    VariableExpression& var = _val(ctx);
    var.type = boost::fusion::at_c<0>(_attr(ctx));
    var.name = boost::fusion::at_c<1>(_attr(ctx));
  };

  x3::rule<class logicalOr, BinaryExpression> const logicalOr{"or"};
  x3::rule<class logicalAnd, BinaryExpression> const logicalAnd{"and"};
  x3::rule<class relational, BinaryExpression> const relational{"relational"};
  x3::rule<class arithmetic, BinaryExpression> const arithmetic{"arithmetic"};
  x3::rule<class logicalNot, UnaryExpression> const logicalNot{"not"};
  x3::rule<class primary, Expression> const primary{"primary"};
  x3::rule<class variable, VariableExpression> const variable{"variable"};
  x3::rule<class constant, ConstantExpression> const constant{"constant"};
  x3::rule<class string, std::string> const string{"string"};
  x3::rule<class identifier, std::string> const identifier{"identifer"};

  // https://stackoverflow.com/questions/49932608/boost-spirit-x3-attribute-does-not-have-the-expected-size-redux
  template<typename T>
  auto const as = [](auto const& p) { return x3::rule<struct tag, T>{"as"} = p; };
  auto const quoteString = [](char ch) { return ch >> x3::no_skip[*~x3::char_(ch)] >> ch; };

  // Keyword parser: ensures keywords aren't followed by alphanumeric or underscore
  auto const keyword = [](auto const& p) { return x3::lexeme[p >> !(x3::alnum | '_')]; };

  // Characters not allowed in unquoted strings
  auto const keyCharSet = x3::char_(R"( "'$@#%!()&|!=~<>+-)");

  auto const logicalOr_def = logicalAnd >> -as<BinaryExpression::Operation>(keyword(logicalOrOperator) >> logicalOr);
  auto const logicalAnd_def = relational >> -as<BinaryExpression::Operation>(keyword(logicalAndOperator) >> logicalAnd);
  auto const relational_def = arithmetic >> -as<BinaryExpression::Operation>(relationalOperator >> relational);
  auto const arithmetic_def = primary >>
                              -as<BinaryExpression::Operation>((arithmeticOperator | x3::attr(Operator::Add)) >>
                                                               arithmetic);

  auto const primary_def = logicalNot | variable | constant | ('(' > logicalOr > ')');
  auto const logicalNot_def = keyword(logicalNotOperator) >> logicalOr;
  auto const variable_def = (varType >> identifier)[setupFieldId];
  auto const constant_def = x3::bool_ | x3::int64 | string;
  auto const string_def = quoteString('\'') | quoteString('\"') | x3::lexeme[+(~keyCharSet)];
  auto const identifier_def = x3::lexeme[(x3::alpha | '_') >> *(x3::alnum | '_')];

  BOOST_SPIRIT_DEFINE(logicalOr,
                      logicalAnd,
                      relational,
                      arithmetic,
                      logicalNot,
                      primary,
                      variable,
                      constant,
                      string,
                      identifier);
}

namespace rs::expr
{
  Expression parse(std::string_view expr)
  {
    auto iter = expr.begin();
    auto end = expr.end();
    x3::unicode::space_type space;
    BinaryExpression binary;

    if (x3::phrase_parse(iter, end, logicalOr, space, binary) && iter == end)
    {
      Expression root{std::move(binary)};
      normalize(root);
      return root;
    }
    else
    {
      RS_THROW_FORMAT(core::Exception, "parsing {} error from [{}]", expr, std::string{iter, end});
    }
  }
}
