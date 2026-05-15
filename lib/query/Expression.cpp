// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

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
      void operator()(std::unique_ptr<BinaryExpression> const& binary)
      {
        if (!binary)
        {
          return;
        }

        normalize(binary->operand);

        if (!binary->optOperation)
        {
          auto extracted = Expression{std::move(binary->operand)};
          root = std::move(extracted);
          return;
        }

        normalize(binary->optOperation->operand);
        shiftAdd(*binary);
      }

      void operator()(std::unique_ptr<UnaryExpression> const& unary)
      {
        if (!unary)
        {
          return;
        }

        normalize(unary->operand);
      }

      void operator()(VariableExpression&) {} // NOLINT(readability-named-parameter)
      void operator()(ConstantExpression&) {} // NOLINT(readability-named-parameter)

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
        shiftAdd(**std::get_if<std::unique_ptr<BinaryExpression>>(&binary.operand));
      }

      Expression& root;
    };
  }

  void normalize(Expression& expr)
  {
    auto normalizer = Normalizer{expr};
    std::visit(normalizer, expr);
  }
}
