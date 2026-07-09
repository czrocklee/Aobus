// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "detail/Normalize.h"
#include <ao/query/Expression.h>

#include <memory>
#include <utility>
#include <variant>

namespace ao::query
{
  namespace
  {
    struct Normalizer final
    {
      void operator()(std::unique_ptr<BinaryExpression> const& binaryPtr)
      {
        if (!binaryPtr)
        {
          return;
        }

        normalize(binaryPtr->operand);

        if (!binaryPtr->optOperation)
        {
          auto extracted = Expression{std::move(binaryPtr->operand)};
          root = std::move(extracted);
          return;
        }

        normalize(binaryPtr->optOperation->operand);
        shiftAdd(*binaryPtr);
      }

      void operator()(std::unique_ptr<UnaryExpression> const& unaryPtr)
      {
        if (!unaryPtr)
        {
          return;
        }

        normalize(unaryPtr->operand);
      }

      void operator()(VariableExpression& /*variable*/) {}
      void operator()(ConstantExpression& /*constant*/) {}
      void operator()(ListExpression& /*list*/) {}
      void operator()(RangeExpression& /*range*/) {}

      void shiftAdd(BinaryExpression& binary)
      {
        if (!binary.optOperation || binary.optOperation->op != Operator::Add)
        {
          return;
        }

        auto* op = &binary.optOperation->operand;
        auto* rhs = std::get_if<std::unique_ptr<BinaryExpression>>(op);

        if (rhs == nullptr || !*rhs)
        {
          return;
        }

        if (!(*rhs)->optOperation || (*rhs)->optOperation->op != Operator::Add)
        {
          return;
        }

        std::swap(binary.operand, (*rhs)->optOperation->operand);
        std::swap((*rhs)->operand, (*rhs)->optOperation->operand);
        std::swap(binary.operand, *op);

        if (auto* next = std::get_if<std::unique_ptr<BinaryExpression>>(&binary.operand); next != nullptr)
        {
          if (*next)
          {
            shiftAdd(**next);
          }
        }
      }

      Expression& root;
    };
  } // namespace

  void normalize(Expression& expr)
  {
    auto normalizer = Normalizer{expr};
    std::visit(normalizer, expr);
  }
} // namespace ao::query
