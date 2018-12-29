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

#include <rs/ml/query/Expression.h>

namespace
{
  using namespace rs::ml::query;

  struct Normalizer
  {
    void operator()(BinaryExpression& binary)
    {
      normalize(binary.operand);

      if (!binary.operation)
      {
        std::swap(root, binary.operand);
      }
      else
      {
        normalize(binary.operation->operand);
      }
    }

    void operator()(UnaryExpression& unary)
    {
      normalize(unary.operand);
    }

    void operator()(VariableExpression& variable)
    {
    }

    void operator()(ConstantExpression& constant)
    {
    }
   
    Expression& root;
  };
}

namespace rs::ml::query
{
  void normalize(Expression& expr)
  {
    Normalizer normalizer{expr};
    boost::apply_visitor(normalizer, expr);
  }
}

