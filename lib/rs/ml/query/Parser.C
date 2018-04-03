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
#include <rs/ml/query/Parser.H>
#include <rs/ml/query/detail/Normalizer.H>
#include <boost/fusion/include/adapt_struct.hpp>
#include <boost/spirit/home/x3.hpp>

BOOST_FUSION_ADAPT_STRUCT(rs::ml::query::BinaryExpression::Operation, op, operand)
BOOST_FUSION_ADAPT_STRUCT(rs::ml::query::BinaryExpression, operand, operations)
BOOST_FUSION_ADAPT_STRUCT(rs::ml::query::UnaryExpression, op, operand)
BOOST_FUSION_ADAPT_STRUCT(rs::ml::query::VariableExpression, name)

namespace
{
  namespace x3 = boost::spirit::x3;
  using namespace rs::ml::query;

  x3::symbols<Operator> logicalAndOperator{{{"and", Operator::And}, {"&&", Operator::And}}};
  x3::symbols<Operator> logicalOrOperator{{{"or", Operator::Or}, {"||", Operator::Or}}};
  x3::symbols<Operator> logicalNotOperator{{{"not", Operator::Not}, {"!", Operator::Not}}};
  x3::symbols<Operator> relationalOperator
  {
    {
      {"=", Operator::Equal},
      {"~", Operator::Like},
      {">", Operator::Greater},
      {"<", Operator::Less}
    }
  };

  const x3::rule<class logicalOr, BinaryExpression> logicalOr{"or"};
  const x3::rule<class logicalAnd, BinaryExpression> logicalAnd{"and"};
  const x3::rule<class relational, BinaryExpression> relational{"relational"};
  const x3::rule<class logicalNot, UnaryExpression>  logicalNot{"not"};
  const x3::rule<class primary, Expression> primary{"primary"};
  const x3::rule<class variable, VariableExpression> variable{"variable"};
  const x3::rule<class constant, ConstantExpression> constant{"constant"};
  const x3::rule<class string, std::string> string{"string"};
 
  const auto logicalOr_def = logicalAnd >> *(logicalOrOperator >> logicalOr);
  const auto logicalAnd_def = relational >> *(logicalAndOperator >> logicalAnd);
  const auto relational_def = primary >> *(relationalOperator >> relational);
  const auto primary_def = constant | variable | ('(' > logicalOr > ')') | logicalNot;
  const auto logicalNot_def = logicalNotOperator >> primary;
  const auto variable_def = '%' >> *(x3::char_ - '%') >> '%';
  const auto constant_def = x3::bool_ | x3::int_ | string;
  const auto string_def = '"' >> x3::no_skip[*~x3::char_('"')] >> '"';

  BOOST_SPIRIT_DEFINE(logicalOr, logicalAnd, relational, logicalNot, primary, variable, constant, string);
}

namespace rs::ml::query
{
  Expression Parser::parse(const std::string& str)
  {
    auto iter = str.begin();
    auto end = str.end();
    x3::unicode::space_type space;
    BinaryExpression binary;
    
    if (x3::phrase_parse(iter, end, logicalOr, space, binary) && iter == end)
    {
      Expression root{std::move(binary)};
      detail::normalize(root);
      return root;
    }
    else
    {
      throw std::runtime_error{std::string(iter, end)};     
    }
  }
}
