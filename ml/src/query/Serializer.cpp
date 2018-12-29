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

#include <rs/ml/query/Serializer.h>
#include <rs/ml/utility/VariantVisitor.h>

#include <sstream>

namespace
{
  using namespace rs::ml::query;

  struct ParenthesisGuard
  {
    ParenthesisGuard(std::ostringstream& oss, bool apply) : oss{oss}, apply{apply} { if (apply) oss << "("; }
    ~ParenthesisGuard() { if (apply) oss << ")"; }
    std::ostringstream& oss;
    bool apply;
  };

  struct Serializer
  {
    Serializer() {};

    void operator()(const BinaryExpression& binary)
    {
      ParenthesisGuard guard{oss, (counter++ > 0) && binary.operation};
      boost::apply_visitor(*this, binary.operand);

      if (binary.operation)
      {
        serializeBinary(binary.operation->op, binary.operation->operand);
      }
    }
    
    void operator()(const UnaryExpression& unary)
    {
      oss << "!";
      boost::apply_visitor(*this, unary.operand);
    }

    void operator()(const VariableExpression& variable)
    {
      switch (variable.type)
      {
        case VariableType::Metadata: oss << '$'; break;
        case VariableType::Property: oss << '@'; break;
        case VariableType::Tag: oss << '#'; break;
        case VariableType::Custom: oss << '%'; break;
        default : break;
      }

      oss << variable.name;
    }

    void operator()(const ConstantExpression& constant)
    {
      std::visit(rs::ml::utility::makeVisitor(
        [](boost::blank) { },
        [this](bool val) { oss << (val ? "true" : "false"); },
        [this](std::int64_t val) { oss << val; },
        [this](std::string_view val) { oss << "\"" << val << "\""; }), constant);
    }

    void serializeBinary(Operator op, const Expression& rhs)
    {
      switch (op)
      {
        case Operator::And: 
          oss << " and "; break;
        case Operator::Or: 
          oss << " or "; break;
        case Operator::Less:
          oss << " < "; break;
        case Operator::LessEqual:
          oss << " <= "; break;
        case Operator::Greater:
          oss << " > "; break;
        case Operator::GreaterEqual:
          oss << " >= "; break;
        case Operator::Equal: 
          oss << " = "; break;
        case Operator::Like: 
          oss << " ~ "; break;

        default: break;
      }

      boost::apply_visitor(*this, rhs);
    }

    std::ostringstream oss;
    std::size_t counter = 0;
  };
}

namespace rs::ml::query
{
  std::string serialize(const Expression& expr)
  {
    Serializer serializer{};
    boost::apply_visitor(serializer, expr);
    return serializer.oss.str();
  }
}

