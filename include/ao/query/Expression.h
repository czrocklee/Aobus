// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

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

  enum class VariableType : std::uint8_t
  {
    Metadata,
    Property,
    Tag,
    Custom
  };

  struct VariableExpression
  {
    VariableType type = VariableType::Metadata;
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

  enum class Operator : std::uint8_t
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
      Operator op = Operator::Equal;
      Expression operand;
    };

    Expression operand;
    std::optional<Operation> optOperation;
  };

  struct UnaryExpression
  {
    Operator op = Operator::Not;
    Expression operand;
  };

  void normalize(Expression& expr);
}
