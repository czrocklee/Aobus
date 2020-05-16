/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <rs/ml/expr/Expression.h>

namespace rs::ml::expr
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

      void operator()(VariableExpression& variable) {}

      void operator()(ConstantExpression& constant) {}

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
