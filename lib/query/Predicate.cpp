// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Expression.h>
#include <ao/query/detail/OperatorTable.h>
#include <ao/query/detail/Predicate.h>
#include <ao/utility/VariantVisitor.h>

#include <cstddef>
#include <memory>
#include <variant>

namespace ao::query::detail
{
  namespace
  {
    bool isPredicateBinary(BinaryExpression const& binary)
    {
      if (!binary.optOperation)
      {
        return isPredicateExpression(binary.operand);
      }

      // Synthetic/out-of-range operators (e.g. from hand-built ASTs) are not predicates;
      // guard before the table lookup, which would otherwise throw on an unknown index.
      if (static_cast<std::size_t>(binary.optOperation->op) >= kOperatorTable.size())
      {
        return false;
      }

      switch (operatorInfo(binary.optOperation->op).cls)
      {
        case OperatorClass::Logical:
          return isPredicateExpression(binary.operand) && isPredicateExpression(binary.optOperation->operand);

        case OperatorClass::Boolean: return true;

        case OperatorClass::Arithmetic:
        case OperatorClass::Unary: return false;
      }

      return false;
    }

    bool isPredicateUnary(UnaryExpression const& unary)
    {
      switch (unary.op)
      {
        case Operator::Exists: return std::holds_alternative<VariableExpression>(unary.operand);
        case Operator::Not: return isPredicateExpression(unary.operand);

        case Operator::And:
        case Operator::Or:
        case Operator::Equal:
        case Operator::NotEqual:
        case Operator::Like:
        case Operator::Less:
        case Operator::LessEqual:
        case Operator::Greater:
        case Operator::GreaterEqual:
        case Operator::In:
        case Operator::Add: return false;
      }

      return false;
    }

    bool isBooleanConstant(ConstantExpression const& constant)
    {
      return std::holds_alternative<bool>(constant);
    }
  } // namespace

  bool isPredicateExpression(Expression const& expr)
  {
    return std::visit(
      utility::makeVisitor(
        [](VariableExpression const& var) { return var.type == VariableType::Tag; },
        [](ConstantExpression const& constant) { return isBooleanConstant(constant); },
        [](ListExpression const&) { return false; },
        [](RangeExpression const&) { return false; },
        [](std::unique_ptr<BinaryExpression> const& binary) { return binary != nullptr && isPredicateBinary(*binary); },
        [](std::unique_ptr<UnaryExpression> const& unary) { return unary != nullptr && isPredicateUnary(*unary); }),
      expr);
  }
} // namespace ao::query::detail
