// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ao::query
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

  struct UnitConstantExpression
  {
    std::string lexeme;
  };

  using ConstantExpression = std::variant<bool, std::int64_t, UnitConstantExpression, std::string>;

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
