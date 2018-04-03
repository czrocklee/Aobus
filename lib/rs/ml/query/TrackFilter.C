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

#include <rs/ml/query/TrackFilter.H>
#include <rs/ml/core/DataValue.H>
#include <rs/ml/utility/VariantVisitor.H>
#include <functional>

namespace
{
  #define ACCESS_ROOT_FIELD()
  using namespace rs::ml::core;
  using namespace rs::ml::query;
  
  #define TRACK_FIELD_ACCESSOR_STR(field) \
  DataValue get##field(const Track::Reader& track) \
  { \
    auto string = track.get##field(); \
    return std::string_view{string.begin(), string.size()};\
  } 

  TRACK_FIELD_ACCESSOR_STR(Title);
  TRACK_FIELD_ACCESSOR_STR(Artist);

  struct Eval
  {
    DataValue operator()(const BinaryExpression& binary)
    {
      DataValue val = evaluate(binary.operand);

      for (const auto& operation : binary.operations)
      {
        val = evaluateBinary(operation.op, val, operation.operand);
      }

      return val;
    }
    
    DataValue operator()(const UnaryExpression& unary)
    {
      assert(unary.op == Operator::Not);
      return !convertToBool(evaluate(unary.operand));
    }

    DataValue operator()(const VariableExpression& variable)
    {
      using FieldAccessor = DataValue(*)(const Track::Reader&);
      static std::map<std::string, FieldAccessor> accessors = {{"title", &getTitle}, {"artist", &getArtist}};
      return std::invoke(accessors[variable.name], track);
    }

    DataValue operator()(const ConstantExpression& constant)
    {
      return std::visit(rs::ml::utility::makeVisitor([](const auto& val) { return DataValue{val}; }), constant);
    }

    static bool convertToBool(const DataValue& val)
    {
      return std::visit(rs::ml::utility::makeVisitor(
        [](boost::blank) { return false; },
        [](bool val) { return val; },
        [](int val) { return static_cast<bool>(val); },
        [](std::string_view) { return true; }), val); 
    }

    bool evaluateBinary(Operator op, const DataValue& lhs, const Expression& rhs)
    {
      switch (op)
      {
        case Operator::And: 
          return convertToBool(lhs) ? convertToBool(evaluate(rhs)) : false;

        case Operator::Or: 
          return convertToBool(lhs) ? true : convertToBool(evaluate(rhs));

        case Operator::Less:
          return std::visit(rs::ml::utility::makeVisitor(
            [](int l, int r) { return l < r; },
            [](std::string_view l, std::string_view r) { return l < r; },
            [](auto, auto) { return false; }), lhs, evaluate(rhs));

        case Operator::Greater:
          return std::visit(rs::ml::utility::makeVisitor(
            [](int l, int r) { return l > r; },
            [](std::string_view l, std::string_view r) { return l > r; },
            [](auto, auto) { return false; }), lhs, evaluate(rhs));

        case Operator::Equal: 
          return lhs == evaluate(rhs);

        case Operator::Like: 
          return std::visit(rs::ml::utility::makeVisitor(
            [](std::string_view l, std::string_view r) { return l.find(r) != std::string_view::npos; },
            [](auto, auto) { return false; }), lhs, evaluate(rhs));

        default: return false;
      }
    }

    DataValue evaluate(const Expression& root)
    {
      return boost::apply_visitor(*this, root);
    }

    const Track::Reader& track;
  };


}


namespace rs::ml::query
{
  struct TrackFilter::Impl
  {
    Impl(Expression&& root) : root{std::move(root)} {}
    Expression root;
  };

  TrackFilter::TrackFilter(Expression&& root)
    : _impl{std::make_shared<Impl>(std::move(root))}
  {
  }

  bool TrackFilter::operator()(const Track::Reader& track) const
  {
    Eval eval{track};
    return Eval::convertToBool(eval.evaluate(_impl->root));
  }
  
  /*
  void TrackFilter::dump() const
  {
  //  _impl->function.compile();
  //  ::jit_dump_function(stdout, _impl->function.raw(), "filter");
  }*/
}

