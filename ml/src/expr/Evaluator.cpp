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

#include <rs/ml/expr/Evaluator.h>
#include <rs/ml/core/Tag.h>
#include <rs/ml/core/Custom.h>
#include <rs/ml/expr/gen/TrackFieldAccessor.h>
#include <rs/common/utility/VariantVisitor.h>
#include <rs/common/Exception.h>

#include <functional>
#include <algorithm>

namespace rs::ml::expr
{
  namespace
  {
    template<typename T>
    class Evaluator
    {
      using DataValue = core::DataValue;

    public:
      T track;

      DataValue operator()(const Expression& root) { return evaluate(root); }

      DataValue operator()(const UnaryExpression& unary)
      {
        assert(unary.op == Operator::Not);
        return !toBool(evaluate(unary.operand));
      }

      DataValue operator()(const VariableExpression& variable)
      {
        if constexpr (std::is_pointer_v<T>)
        {
          if (track == nullptr)
          {
            RS_THROW(common::Exception, "Invalid access to variable type where track is not provided"); 
          }
        }

        switch (variable.type)
        {
          case VariableType::Metadata: return gen::MetadataAccessor::get(track, variable.fieldId);
          case VariableType::Property: return gen::PropertyAccessor::get(track, variable.fieldId);
          case VariableType::Tag: return rs::ml::core::tag(track, variable.name.c_str());
          case VariableType::Custom: return rs::ml::core::custom(track, variable.name.c_str());
          default: return {};
        }
      }

      DataValue operator()(const ConstantExpression& constant)
      {
        return std::visit(rs::common::utility::makeVisitor([](const auto& val) { return DataValue{val}; }), constant);
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
      static std::string stringCat(core::DataValue&& lhs, core::DataValue&& rhs)
      {
        std::string value = std::visit(
          rs::common::utility::makeVisitor(
            [](std::monostate) { return std::string{"nil"}; },
            [](bool val) { return std::string{val ? "true" : "false"}; },
            [](std::int64_t val) { return std::to_string(val); },
            [](std::string_view val) { return std::string{val}; },
            [](std::string&& val) { return std::string{std::move(val)}; }),
          std::move(lhs));

        std::visit(
          rs::common::utility::makeVisitor(
            [&value](std::monostate) { value.append("nil"); },
            [&value](bool val) { value.append(val ? "true" : "false"); },
            [&value](std::int64_t val) { value.append(std::to_string(val)); },
            [&value](std::string_view val) { value.append(val); },
            [&value](std::string&& val) { value.append(val); }),
          std::move(rhs));

        return value;
      }

      DataValue evaluateBinary(Operator op, DataValue&& lhs, const Expression& rhs)
      {
#define RELATIONAL(lhs, op, rhs)                                                                                               \
  std::visit(                                                                                                                  \
    rs::common::utility::makeVisitor(                                                                                          \
      [](std::int64_t l, std::int64_t r) { return DataValue{l op r}; },                                                                   \
      [](std::string_view l, std::string_view r) { return DataValue{l op r}; },                                                           \
      [](std::string&& l, std::string&& r) { return DataValue{l op r}; },                                                       \
      [](auto&&, auto&&) { return DataValue{false}; }),                                                                                     \
    lhs,                                                                                                                       \
    evaluate(rhs));

        switch (op)
        {
          case Operator::And: return toBool(lhs) ? toBool(evaluate(rhs)) : false;
          case Operator::Or: return toBool(lhs) ? true : toBool(evaluate(rhs));
          case Operator::Equal: return lhs == evaluate(rhs);
          case Operator::Like:
            return std::visit(
              rs::common::utility::makeVisitor(
                [](std::string_view l, std::string_view r) { return DataValue{l.find(r) != std::string_view::npos}; },
                [](std::string_view l, std::string&& r) { return DataValue{l.find(r) != std::string_view::npos}; },
                [](std::string&& l, std::string_view r) { return DataValue{l.find(r) != std::string_view::npos}; },
                [](std::string&& l, std::string&& r) { return DataValue{l.find(r) != std::string_view::npos}; },
                [](auto&&, auto&&) { return DataValue{false}; }),
              std::move(lhs),
              evaluate(rhs)); 
          case Operator::Less: return RELATIONAL(lhs, <, rhs);
          case Operator::LessEqual: return RELATIONAL(lhs, <=, rhs);
          case Operator::Greater: return RELATIONAL(lhs, >, rhs);
          case Operator::GreaterEqual: return RELATIONAL(lhs, >=, rhs);
          case Operator::Add:
            return std::visit(
              rs::common::utility::makeVisitor(
                [](std::int64_t&& l, std::int64_t&& r) { return DataValue{l + r}; },
                [](auto&& l, auto&& r) { return DataValue{stringCat(std::move(l), std::move(r))}; }),
              std::move(lhs),
              evaluate(rhs)); 

          default: return false;
        }
      }

      DataValue evaluate(const Expression& root) { return boost::apply_visitor(*this, root); }
    };
  }

  bool toBool(const core::DataValue& val)
  {
    return std::visit(
      rs::common::utility::makeVisitor(
        [](std::monostate) { return false; },
        [](bool val) { return val; },
        [](std::int64_t val) { return static_cast<bool>(val); },
        [](std::string_view) { return true; },
        [](std::string&&) { return true; }),
      val);
  }

  core::DataValue evaluate(const Expression& expr) { return std::invoke(Evaluator<const fbs::Track*>{nullptr}, expr); }

  core::DataValue evaluate(const Expression& expr, const fbs::Track* track)
  {
    return std::invoke(Evaluator<const fbs::Track*>{track}, expr);
  }

  core::DataValue evaluate(const Expression& expr, const fbs::TrackT& track)
  {
    return std::invoke(Evaluator<const fbs::TrackT&>{track}, expr);
  }
}
