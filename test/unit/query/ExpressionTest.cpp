// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/query/Expression.h>
#include <ao/utility/VariantVisitor.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>

namespace ao::query::test
{
  namespace
  {
    // Local canonicalizer for AST comparison without relying on Parser/Serializer
    struct Canonicalizer final
    {
      void operator()(std::unique_ptr<BinaryExpression> const& binary)
      {
        if (!binary)
        {
          oss << "null";
          return;
        }

        oss << "(";
        std::visit(*this, binary->operand);

        if (binary->optOperation)
        {
          switch (binary->optOperation->op)
          {
            case Operator::Add: oss << " + "; break;
            case Operator::And: oss << " and "; break;
            case Operator::Or: oss << " or "; break;
            case Operator::Equal: oss << " = "; break;
            case Operator::NotEqual: oss << " != "; break;
            case Operator::Like: oss << " ~ "; break;
            case Operator::Less: oss << " < "; break;
            case Operator::LessEqual: oss << " <= "; break;
            case Operator::Greater: oss << " > "; break;
            case Operator::GreaterEqual: oss << " >= "; break;
            default: oss << " op "; break;
          }

          std::visit(*this, binary->optOperation->operand);
        }

        oss << ")";
      }

      void operator()(std::unique_ptr<UnaryExpression> const& unary)
      {
        if (!unary)
        {
          oss << "null";
          return;
        }

        oss << "not ";
        std::visit(*this, unary->operand);
      }

      void operator()(VariableExpression const& var) { oss << var.name; }

      void operator()(ConstantExpression const& constant)
      {
        std::visit(utility::makeVisitor([this](bool val) { oss << (val ? "true" : "false"); },
                                        [this](std::int64_t val) { oss << val; },
                                        [this](UnitConstantExpression const& val) { oss << val.lexeme; },
                                        [this](std::string const& val) { oss << "\"" << val << "\""; }),
                   constant);
      }

      std::ostringstream oss;
    };

    std::string canonicalize(Expression const& expr)
    {
      auto c = Canonicalizer{};
      std::visit(c, expr);
      return c.oss.str();
    }
  }

  TEST_CASE("Expression - Normalize Collapses Binary Node Without Operation", "[query][unit][expression]")
  {
    // Input: (a) where (a) is a BinaryExpression with no optOperation
    auto binaryPtr = std::make_unique<BinaryExpression>();
    binaryPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "a"};
    binaryPtr->optOperation = std::nullopt;

    auto expr = Expression{std::move(binaryPtr)};

    normalize(expr);

    CHECK(canonicalize(expr) == "a");
  }

  TEST_CASE("Expression - Normalize Leaves Constant Unchanged", "[query][unit][expression]")
  {
    auto expr = Expression{ConstantExpression{true}};
    normalize(expr);
    CHECK(canonicalize(expr) == "true");
  }

  TEST_CASE("Expression - Normalize Leaves Variable Unchanged", "[query][unit][expression]")
  {
    auto expr = Expression{VariableExpression{.type = VariableType::Metadata, .name = "artist"}};
    normalize(expr);
    CHECK(canonicalize(expr) == "artist");
  }

  TEST_CASE("Expression - Normalize Reassociates Right Nested Add Chain", "[query][unit][expression]")
  {
    // Input: a + (b + c)
    auto innerPtr = std::make_unique<BinaryExpression>();
    innerPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "b"};
    innerPtr->optOperation = BinaryExpression::Operation{
      .op = Operator::Add, .operand = VariableExpression{.type = VariableType::Metadata, .name = "c"}};

    auto rootPtr = std::make_unique<BinaryExpression>();
    rootPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "a"};
    rootPtr->optOperation = BinaryExpression::Operation{.op = Operator::Add, .operand = std::move(innerPtr)};

    auto expr = Expression{std::move(rootPtr)};
    normalize(expr);

    CHECK(canonicalize(expr) == "((a + b) + c)");
  }

  TEST_CASE("Expression - Normalize Reassociates Four Term Add Chain", "[query][unit][expression]")
  {
    // Input: a + (b + (c + d))
    auto innermostPtr = std::make_unique<BinaryExpression>();
    innermostPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "c"};
    innermostPtr->optOperation = BinaryExpression::Operation{
      .op = Operator::Add, .operand = VariableExpression{.type = VariableType::Metadata, .name = "d"}};

    auto innerPtr = std::make_unique<BinaryExpression>();
    innerPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "b"};
    innerPtr->optOperation = BinaryExpression::Operation{.op = Operator::Add, .operand = std::move(innermostPtr)};

    auto rootPtr = std::make_unique<BinaryExpression>();
    rootPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "a"};
    rootPtr->optOperation = BinaryExpression::Operation{.op = Operator::Add, .operand = std::move(innerPtr)};

    auto expr = Expression{std::move(rootPtr)};
    normalize(expr);

    CHECK(canonicalize(expr) == "(((a + b) + c) + d)");
  }

  TEST_CASE("Expression - Normalize Does Not Touch NonAdd Binary", "[query][unit][expression]")
  {
    // Input: a and (b and c)
    auto innerPtr = std::make_unique<BinaryExpression>();
    innerPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "b"};
    innerPtr->optOperation = BinaryExpression::Operation{
      .op = Operator::And, .operand = VariableExpression{.type = VariableType::Metadata, .name = "c"}};

    auto rootPtr = std::make_unique<BinaryExpression>();
    rootPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "a"};
    rootPtr->optOperation = BinaryExpression::Operation{.op = Operator::And, .operand = std::move(innerPtr)};

    auto expr = Expression{std::move(rootPtr)};
    normalize(expr);

    CHECK(canonicalize(expr) == "(a and (b and c))");
  }

  TEST_CASE("Expression - Normalize Stops When Right Operand Is Not Binary", "[query][unit][expression]")
  {
    // Input: a + 1
    auto rootPtr = std::make_unique<BinaryExpression>();
    rootPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "a"};
    rootPtr->optOperation =
      BinaryExpression::Operation{.op = Operator::Add, .operand = ConstantExpression{std::int64_t{1}}};

    auto expr = Expression{std::move(rootPtr)};
    normalize(expr);

    CHECK(canonicalize(expr) == "(a + 1)");
  }

  TEST_CASE("Expression - Normalize Stops When Right Binary Is Not Add", "[query][unit][expression]")
  {
    // Input: a + (b and c)
    auto innerPtr = std::make_unique<BinaryExpression>();
    innerPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "b"};
    innerPtr->optOperation = BinaryExpression::Operation{
      .op = Operator::And, .operand = VariableExpression{.type = VariableType::Metadata, .name = "c"}};

    auto rootPtr = std::make_unique<BinaryExpression>();
    rootPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "a"};
    rootPtr->optOperation = BinaryExpression::Operation{.op = Operator::Add, .operand = std::move(innerPtr)};

    auto expr = Expression{std::move(rootPtr)};
    normalize(expr);

    CHECK(canonicalize(expr) == "(a + (b and c))");
  }

  TEST_CASE("Expression - Normalize Unary Recurses Into Operand", "[query][unit][expression]")
  {
    // Input: not (a + (b + c))
    auto innerPtr = std::make_unique<BinaryExpression>();
    innerPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "b"};
    innerPtr->optOperation = BinaryExpression::Operation{
      .op = Operator::Add, .operand = VariableExpression{.type = VariableType::Metadata, .name = "c"}};

    auto binaryPtr = std::make_unique<BinaryExpression>();
    binaryPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "a"};
    binaryPtr->optOperation = BinaryExpression::Operation{.op = Operator::Add, .operand = std::move(innerPtr)};

    auto unaryPtr = std::make_unique<UnaryExpression>();
    unaryPtr->op = Operator::Not;
    unaryPtr->operand = std::move(binaryPtr);

    auto expr = Expression{std::move(unaryPtr)};
    normalize(expr);

    CHECK(canonicalize(expr) == "not ((a + b) + c)");
  }

  TEST_CASE("Expression - Normalize Null Unary Pointer Is Safe", "[query][unit][expression]")
  {
    auto expr = Expression{std::unique_ptr<UnaryExpression>{}};
    normalize(expr);
    CHECK(canonicalize(expr) == "null");
  }

  TEST_CASE("Expression - Normalize Null Binary Pointer Is Safe", "[query][unit][expression]")
  {
    auto expr = Expression{std::unique_ptr<BinaryExpression>{}};
    normalize(expr);
    CHECK(canonicalize(expr) == "null");
  }
} // namespace ao::query::test
