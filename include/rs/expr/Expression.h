// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <boost/spirit/home/x3/support/ast/variant.hpp>

#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <variant>
#include <vector>

namespace rs::expr
{
  struct BinaryExpression;
  struct UnaryExpression;

  enum class VariableType
  {
    Metadata,
    Property,
    Tag,
    Custom
  };

  struct VariableExpression
  {
    VariableType type;
    std::string name;
  };

  using ConstantExpression = std::variant<bool, std::int64_t, std::string>;

  // forward_ast is used for recursive types in the expression variant.
  // Note: boost::spirit x3 with forward_ast in recursive variants has a known
  // memory leak issue in ASAN due to how the variant's convert_construct
  // handles recursive types. This is a library limitation, not our code.
  using Expression = boost::spirit::x3::variant<VariableExpression,
                                                ConstantExpression,
                                                boost::spirit::x3::forward_ast<BinaryExpression>,
                                                boost::spirit::x3::forward_ast<UnaryExpression>>;

  enum class Operator
  {
    And,
    Or,
    Not,
    Equal,
    NotEqual,
    Like,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Add
  };

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
