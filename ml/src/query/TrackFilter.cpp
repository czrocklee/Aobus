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

#include <rs/ml/query/TrackFilter.h>
#include <rs/ml/core/DataValue.h>
#include <rs/ml/core/Tag.h>
#include <rs/ml/core/Custom.h>
#include <rs/ml/utility/VariantVisitor.h>
#include <rs/ml/query/gen/TrackFieldAccessor.h>

#include <functional>
#include <algorithm>

namespace
{
  using namespace rs::ml::core;
  using namespace rs::ml::query;
 
  template<typename Track>
  class Evaluator
  { 
  public:
    Track track;

    bool operator()(const Expression& root)
    {
      return convertToBool(evaluate(root));
    }

    DataValue operator()(const UnaryExpression& unary)
    {
      assert(unary.op == Operator::Not);
      return !convertToBool(evaluate(unary.operand));
    }
    
    DataValue operator()(const VariableExpression& variable)
    {
      switch (variable.type)
      {
        case VariableType::Metadata:
          return gen::MetadataAccessor::get(track, variable.fieldId);
        case VariableType::Property:
          return gen::PropertyAccessor::get(track, variable.fieldId);
        case VariableType::Tag:
          return rs::ml::core::tag(track, variable.name.c_str());
        case VariableType::Custom: 
          return rs::ml::core::custom(track, variable.name.c_str());
        default : return{};
      }
    }

    DataValue operator()(const ConstantExpression& constant)
    {
      return std::visit(rs::ml::utility::makeVisitor([](const auto& val) { return DataValue{val}; }), constant);
    }

    DataValue operator()(const BinaryExpression& binary)
    {
      DataValue val = evaluate(binary.operand);

      if (binary.operation)
      {
        val = evaluateBinary(binary.operation->op, std::move(val), binary.operation->operand);
      }

      return val;
    }

  private:
    bool evaluateBinary(Operator op, DataValue&& lhs, const Expression& rhs)
    {
#define RELATIONAL(lhs, op, rhs) \
  std::visit(rs::ml::utility::makeVisitor( \
    [](std::int64_t l, std::int64_t r) { return l op r; }, \
    [](std::string_view l, std::string_view r) { return l op r; }, \
    [](auto&, auto&) { return false; }), lhs, evaluate(rhs));

      switch (op)
      {
        case Operator::And: 
          return convertToBool(lhs) ? convertToBool(evaluate(rhs)) : false;
        case Operator::Or: 
          return convertToBool(lhs) ? true : convertToBool(evaluate(rhs));
        case Operator::Equal: 
          return lhs == evaluate(rhs);
        case Operator::Like: 
          return std::visit(rs::ml::utility::makeVisitor(
            [](std::string_view l, std::string_view r) { return l.find(r) != std::string_view::npos; },
            [](auto&, auto&) { return false; }), lhs, evaluate(rhs));
        case Operator::Less:
          return RELATIONAL(lhs, <, rhs);
        case Operator::LessEqual:
          return RELATIONAL(lhs, <=, rhs);
        case Operator::Greater:
          return RELATIONAL(lhs, >, rhs);
        case Operator::GreaterEqual:
          return RELATIONAL(lhs, >=, rhs);
    
        default: return false;
      }
    }

    static bool convertToBool(const DataValue& val)
    {
      return std::visit(rs::ml::utility::makeVisitor(
        [](boost::blank) { return false; },
        [](bool val) { return val; },
        [](std::int64_t val) { return static_cast<bool>(val); },
        [](std::string_view) { return true; }), val);
    }

    const DataValue evaluate(const Expression& root)
    {
      return boost::apply_visitor(*this, root);
    }

  };
}

namespace rs::ml::query
{
  TrackFilter::TrackFilter(Expression expr)
    : _expr{std::move(expr)}
  {
  }

  bool TrackFilter::operator()(const Track* track) const
  {
    return std::invoke(Evaluator<const Track*>{track}, _expr);
  }
  
  bool TrackFilter::operator()(const TrackT& track) const
  {
    return std::invoke(Evaluator<const TrackT&>{track}, _expr);
  }
}

