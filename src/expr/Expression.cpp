// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/expr/Expression.h>

namespace rs::expr
{
  namespace
  {
    struct Normalizer
    {
      void operator()(BinaryExpression& binary)
      {
        normalize(binary.operand);

        if (!binary.operation)
        {
          std::swap(root, binary.operand);
          return;
        }

        normalize(binary.operation->operand);
        shiftAdd(binary);
      }

      void operator()(UnaryExpression& unary) { normalize(unary.operand); }

      void operator()(VariableExpression&) {}

      void operator()(ConstantExpression&) {}

      void shiftAdd(BinaryExpression& binary)
      {
        if (binary.operation->op == Operator::Add)
        {
          if (auto* rhs = boost::get<boost::spirit::x3::forward_ast<BinaryExpression>>(&binary.operation->operand);
              rhs != nullptr && rhs->get().operation && rhs->get().operation->op == Operator::Add)
          {
            std::swap(binary.operand, rhs->get().operation->operand);
            std::swap(rhs->get().operand, rhs->get().operation->operand);
            std::swap(binary.operand, binary.operation->operand);
            shiftAdd(boost::get<boost::spirit::x3::forward_ast<BinaryExpression>>(binary.operand).get());
          }
        }
      }

      Expression& root;
    };
  }

  void normalize(Expression& expr)
  {
    Normalizer normalizer{expr};
    boost::apply_visitor(normalizer, expr);
  }
}
