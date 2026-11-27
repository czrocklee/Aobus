// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/query/Serializer.h>

#include "detail/OperatorTable.h"
#include <ao/query/Expression.h>
#include <ao/utility/String.h>
#include <ao/utility/VariantVisitor.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ao::query
{
  namespace
  {
    bool isSimpleUserVariableName(std::string_view name)
    {
      return !name.empty() &&
             std::ranges::all_of(name, [](char ch) { return utility::isAsciiAlphaNumeric(ch) || ch == '_'; });
    }

    void appendQuotedString(std::ostringstream& oss, std::string_view str)
    {
      oss.put('"');

      for (auto const ch : str)
      {
        switch (ch)
        {
          case '\\': oss << "\\\\"; break;
          case '"': oss << "\\\""; break;
          case '\n': oss << "\\n"; break;
          case '\t': oss << "\\t"; break;
          case '\r': oss << "\\r"; break;
          default: oss.put(ch); break;
        }
      }

      oss.put('"');
    }

    void serializeUserVariableName(std::ostringstream& oss, std::string_view name)
    {
      if (isSimpleUserVariableName(name))
      {
        oss << name;
        return;
      }

      appendQuotedString(oss, name);
    }

    struct Serializer final
    {
      Serializer() = default;

      void operator()(std::unique_ptr<BinaryExpression> const& binaryPtr)
      {
        if (!binaryPtr)
        {
          return;
        }

        auto const needsParens = (counter++ > 0) && binaryPtr->optOperation;

        if (needsParens)
        {
          oss.put('(');
        }

        std::visit(*this, binaryPtr->operand);

        if (binaryPtr->optOperation)
        {
          serializeBinary(binaryPtr->optOperation->op, binaryPtr->optOperation->operand);
        }

        if (needsParens)
        {
          oss.put(')');
        }
      }

      void operator()(std::unique_ptr<UnaryExpression> const& unaryPtr)
      {
        if (!unaryPtr)
        {
          return;
        }

        if (unaryPtr->op == Operator::Exists)
        {
          auto const needsParens = std::get_if<VariableExpression>(&unaryPtr->operand) == nullptr;

          if (needsParens)
          {
            oss.put('(');
          }

          std::visit(*this, unaryPtr->operand);

          if (needsParens)
          {
            oss.put(')');
          }

          oss.put('?');
          return;
        }

        oss << "not ";
        // A binary operand is nested even when this negation is the root.
        ++counter;
        std::visit(*this, unaryPtr->operand);
      }

      void operator()(VariableExpression const& variable)
      {
        switch (variable.type)
        {
          case VariableType::Metadata: oss << '$' << variable.name; return;
          case VariableType::Property: oss << '@' << variable.name; return;
          case VariableType::Tag: oss.put('#'); break;
          case VariableType::Custom: oss.put('%'); break;
        }

        serializeUserVariableName(oss, variable.name);
      }

      void operator()(ConstantExpression const& constant) { serializeConstant(constant); }

      void operator()(ListExpression const& list)
      {
        oss.put('[');

        bool first = true;

        for (auto const& value : list.values)
        {
          if (!first)
          {
            oss << ", ";
          }

          serializeConstant(value);
          first = false;
        }

        oss.put(']');
      }

      void operator()(RangeExpression const& range)
      {
        serializeConstant(range.lower);
        oss << "..";
        serializeConstant(range.upper);
      }

      void serializeBinary(Operator op, Expression const& rhs)
      {
        oss << ' ' << detail::operatorDescriptor(op).spelling << ' ';
        std::visit(*this, rhs);
      }

      void serializeConstant(ConstantExpression const& constant)
      {
        std::visit(utility::makeVisitor([this](bool val) { oss << (val ? "true" : "false"); },
                                        [this](std::int64_t val) { oss << std::format("{}", val); },
                                        [this](UnitConstantExpression const& val) { oss << val.lexeme; },
                                        [this](std::string_view val) { appendQuotedString(oss, val); }),
                   constant);
      }

      std::ostringstream oss;
      std::size_t counter = 0;
    };
  } // namespace

  std::string serialize(Expression const& expr)
  {
    auto serializer = Serializer{};
    std::visit(serializer, expr);
    return std::move(serializer.oss).str();
  }
} // namespace ao::query
