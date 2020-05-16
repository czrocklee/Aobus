/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define BOOST_SPIRIT_X3_UNICODE
#include <rs/ml/expr/Parser.h>
#include <rs/ml/expr/gen/TrackFieldAccessor.h>
#include <rs/common/Exception.h>

#include <boost/fusion/include/adapt_struct.hpp>
#include <boost/spirit/home/x3.hpp>
#include <rs/ml/expr/Serializer.h>
#include <iostream>

BOOST_FUSION_ADAPT_STRUCT(rs::ml::expr::BinaryExpression::Operation, op, operand)
BOOST_FUSION_ADAPT_STRUCT(rs::ml::expr::BinaryExpression, operand, operation)
BOOST_FUSION_ADAPT_STRUCT(rs::ml::expr::UnaryExpression, op, operand)
BOOST_FUSION_ADAPT_STRUCT(rs::ml::expr::VariableExpression, type, name)

namespace
{
  namespace x3 = boost::spirit::x3;
  using namespace rs::ml::expr;

  const x3::symbols<VariableType> varType 
  {
    {
      {"$", VariableType::Metadata},
      {"@", VariableType::Property},
      {"#", VariableType::Tag},
      {"%", VariableType::Custom},
    }
  };

  const x3::symbols<Operator> logicalAndOperator{{{"and", Operator::And}, {"&&", Operator::And}}};
  const x3::symbols<Operator> logicalOrOperator{{{"or", Operator::Or}, {"||", Operator::Or}}};
  const x3::symbols<Operator> logicalNotOperator{{{"not", Operator::Not}, {"!", Operator::Not}}};
  const x3::symbols<Operator> arithmeticOperator{{{"+", Operator::Add}}};
  const x3::symbols<Operator> strcatOperator{{{"", Operator::Add}}};
  const x3::symbols<Operator> relationalOperator
  {
    {
      {"=", Operator::Equal},
      {"~", Operator::Like},
      {"<", Operator::Less},
      {"<=", Operator::LessEqual},
      {">", Operator::Greater},
      {">=", Operator::GreaterEqual}
    }
  };

  auto setupFieldId = [](auto& ctx) 
  {
    VariableExpression& var = _val(ctx);
    var.type = boost::fusion::at_c<0>(_attr(ctx));
    var.name = boost::fusion::at_c<1>(_attr(ctx));

    if (var.type == VariableType::Metadata)
    {
      var.fieldId = gen::MetadataAccessor::name2Id(var.name);
    }
    else if (var.type == VariableType::Property)
    {
      var.fieldId = gen::PropertyAccessor::name2Id(var.name);
    }
  };

  const x3::rule<class logicalOr, BinaryExpression> logicalOr{"or"};
  const x3::rule<class logicalAnd, BinaryExpression> logicalAnd{"and"};
  const x3::rule<class relational, BinaryExpression> relational{"relational"};
  const x3::rule<class arithmetic, BinaryExpression> arithmetic{"arithmetic"};
  const x3::rule<class logicalNot, UnaryExpression>  logicalNot{"not"};
  const x3::rule<class primary, Expression> primary{"primary"};
  const x3::rule<class variable, VariableExpression> variable{"variable"};
  const x3::rule<class constant, ConstantExpression> constant{"constant"};
  const x3::rule<class string, std::string> string{"string"};
  const x3::rule<class identifier, std::string> identifier{"identifer"};

  //https://stackoverflow.com/questions/49932608/boost-spirit-x3-attribute-does-not-have-the-expected-size-redux
  template <typename T> 
  const auto as = [](auto p) { return x3::rule<struct tag, T> {"as"} = p; };
  const auto quoteString = [](char ch) { return ch >> x3::no_skip[*~x3::char_(ch)] >> ch; };
  const auto keyCharSet = x3::char_(R"( "'$@#%!()&|!=~<>+)");
 
  const auto logicalOr_def = logicalAnd >> -as<BinaryExpression::Operation>(logicalOrOperator >> logicalOr);
  const auto logicalAnd_def = relational >> -as<BinaryExpression::Operation>(logicalAndOperator >> logicalAnd);
  const auto relational_def = arithmetic >> -as<BinaryExpression::Operation>(relationalOperator >> relational);
  const auto arithmetic_def = primary >> -as<BinaryExpression::Operation>((arithmeticOperator | x3::attr(Operator::Add)) >> arithmetic);
 
  const auto primary_def =  logicalNot | variable | constant | ('(' > logicalOr > ')');
  const auto logicalNot_def = logicalNotOperator >> logicalOr;
  const auto variable_def = (varType >> identifier)[setupFieldId];
  const auto constant_def = x3::bool_ | x3::int64 | string;
  const auto string_def = quoteString('\'') | quoteString('\"') | x3::lexeme[+(~keyCharSet)];
  const auto identifier_def = x3::lexeme[(x3::alpha | '_') >> *(x3::alnum | '_')];
  
  BOOST_SPIRIT_DEFINE(logicalOr, logicalAnd, relational, arithmetic, logicalNot, primary, variable, constant, string, identifier);
}

namespace rs::ml::expr
{
  Expression parse(const std::string& expr)
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
      std::ostringstream oss;
      oss << "parsing " << expr << " error from [" << std::string{iter, end} << ']';
      RS_THROW_FORMAT(common::Exception, "parsing {} error from [{}]", expr, std::string{iter, end});
    }
  }
}
