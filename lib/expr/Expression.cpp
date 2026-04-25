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
        if (!binary)
        {
          return;
        }
        
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
        if (!binary.operation || binary.operation->op != Operator::Add)
        {
          return;
        }
        
        auto* op = &binary.operation->operand;
        auto* rhs = std::get_if<std::unique_ptr<BinaryExpression>>(op);
        
        if (rhs == nullptr || !*rhs)
        {
          return;
        }
        
        if (!(*rhs)->operation || (*rhs)->operation->op != Operator::Add)
        {
          return;
        }
        
        std::swap(binary.operand, (*rhs)->operation->operand);
        std::swap((*rhs)->operand, (*rhs)->operation->operand);
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
