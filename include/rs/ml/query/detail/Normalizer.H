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

#pragma once

#include <rs/ml/query/Expression.H>

namespace rs::ml::query::detail
{
  void normalize(Expression& root);

  struct Normalizer
  {
    void operator()(BinaryExpression& binary)
    {
      normalize(binary.operand);

      if (binary.operations.empty())
      {
        std::swap(root, binary.operand);
      }

      for (auto& operation : binary.operations)
      {
        normalize(operation.operand);
      }
    }

    void operator()(UnaryExpression& unary)
    {
      normalize(unary.operand);
    }

    void operator()(VariableExpression&)
    {
    }

    void operator()(ConstantExpression&)
    {
    }
    
    Expression& root;
  };

  inline void normalize(Expression& root)
  {
    Normalizer normalizer{root};
    boost::apply_visitor(normalizer, root);
  }
}
