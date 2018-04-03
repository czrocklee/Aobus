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

#include <variant>
#include <string>
#include <vector>
#include <regex>

namespace rs::ml::query
{
  struct BinaryExpression;
  struct UnaryExpression;
  struct VariableExpression;
  using ConstantExpression = std::variant<bool, int, std::string>;

  using Expression = boost::spirit::x3::variant<
    ConstantExpression,
    boost::spirit::x3::forward_ast<BinaryExpression>,
    boost::spirit::x3::forward_ast<UnaryExpression>,
    boost::spirit::x3::forward_ast<VariableExpression>
  >;

  enum class Operator { And, Or, Not, Equal, Less, Greater, GreaterEqual, LessEqual, Like };

  struct BinaryExpression
  {
    struct Operation
    {
      Operator op;
      Expression operand;
    };

    Expression operand;
    std::vector<Operation> operations;
  };

  struct UnaryExpression
  {
    Operator op;
    Expression operand;
  };

  struct VariableExpression
  {
    std::string name;
  };
}


