// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
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

  // Expression uses std::unique_ptr for recursive types to avoid the memory leak issue
  // that occurs with boost::spirit x3's forward_ast in recursive variants.
  using Expression = std::variant<VariableExpression,
                                 ConstantExpression,
                                 std::unique_ptr<BinaryExpression>,
                                 std::unique_ptr<UnaryExpression>>;

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
