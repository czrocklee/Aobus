// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/expr/Expression.h>

namespace rs::expr
{
  namespace
  {
    struct Normalizer
    {
      void operator()(std::unique_ptr<BinaryExpression> const& binary)
      {
        if (!binary) return;
        normalize(binary->operand);

        if (!binary->operation)
        {
          std::swap(root, binary->operand);
          return;
        }

        normalize(binary->operation->operand);
        shiftAdd(*binary);
      }

      void operator()(std::unique_ptr<UnaryExpression> const& unary)
      {
        if (!unary) return;
        normalize(unary->operand);
      }

      void operator()(VariableExpression&) {}

      void operator()(ConstantExpression&) {}

      void shiftAdd(BinaryExpression& binary)
      {
        if (binary.operation->op == Operator::Add)
        {
          if (auto* rhs = std::get_if<std::unique_ptr<BinaryExpression>>(&binary.operation->operand);
              rhs != nullptr && *rhs != nullptr && (*rhs)->operation && (*rhs)->operation->op == Operator::Add)
          {
            std::swap(binary.operand, (*rhs)->operation->operand);
            std::swap((*rhs)->operand, (*rhs)->operation->operand);
            std::swap(binary.operand, binary.operation->operand);
            shiftAdd(**std::get_if<std::unique_ptr<BinaryExpression>>(&binary.operand));
          }
        }
      }

      Expression& root;
    };
  }

  void normalize(Expression& expr)
  {
    Normalizer normalizer{expr};
    std::visit(normalizer, expr);
  }
}
