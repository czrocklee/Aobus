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

#pragma once

#include <boost/spirit/home/x3/support/ast/variant.hpp>

#include <optional>
#include <variant>
#include <string>
#include <vector>
#include <regex>

namespace rs::ml::expr
{
  struct BinaryExpression;
  struct UnaryExpression;
  
  enum class VariableType { Metadata, Property, Tag, Custom };

  struct VariableExpression
  {
    VariableType type;
    std::string name;
    int fieldId = -1; // fieldId to rs::ml::fbs::Track 
  };

  using ConstantExpression = std::variant<bool, std::int64_t, std::string>;

  using Expression = boost::spirit::x3::variant<
    VariableExpression,
    ConstantExpression,
    boost::spirit::x3::forward_ast<BinaryExpression>,
    boost::spirit::x3::forward_ast<UnaryExpression>
  >;

  enum class Operator { And, Or, Not, Equal, Like, Less, LessEqual, Greater, GreaterEqual, Add };

  struct BinaryExpression
  {
    struct Operation
    {
      Operator op;
      Expression operand;
    };

    Expression operand;
    std::optional<Operation> operation;
  };

  struct UnaryExpression
  {
    Operator op;
    Expression operand;
  };

  void normalize(Expression& expr);
}
