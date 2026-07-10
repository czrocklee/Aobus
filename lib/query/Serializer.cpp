// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/query/detail/OperatorTable.h>
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
      return !name.empty() && std::ranges::all_of(name,
                                                  [](char ch)
                                                  {
                                                    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                                           (ch >= '0' && ch <= '9') || ch == '_';
                                                  });
    }

    template<typename... Args>
    void appendFormatted(std::ostringstream& oss, std::format_string<Args...> fmt, Args&&... args)
    {
      oss << std::format(fmt, std::forward<Args>(args)...);
    }

    void appendEscapedString(std::ostringstream& oss, std::string_view str)
    {
      for (auto const ch : str)
      {
        switch (ch)
        {
          case '\\': appendFormatted(oss, "\\\\"); break;
          case '"': appendFormatted(oss, "\\\""); break;
          case '\n': appendFormatted(oss, "\\n"); break;
          case '\t': appendFormatted(oss, "\\t"); break;
          case '\r': appendFormatted(oss, "\\r"); break;
          default: appendFormatted(oss, "{}", ch); break;
        }
      }
    }

    void serializeUserVariableName(std::ostringstream& oss, std::string_view name)
    {
      if (isSimpleUserVariableName(name))
      {
        appendFormatted(oss, "{}", name);
        return;
      }

      appendFormatted(oss, "\"");
      appendEscapedString(oss, name);
      appendFormatted(oss, "\"");
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

          appendFormatted(oss, "?");
          return;
        }

        appendFormatted(oss, "not ");
        std::visit(*this, unaryPtr->operand);
      }

      void operator()(VariableExpression const& variable)
      {
        switch (variable.type)
        {
          case VariableType::Metadata: appendFormatted(oss, "${}", variable.name); return;
          case VariableType::Property: appendFormatted(oss, "@{}", variable.name); return;
          case VariableType::Tag: appendFormatted(oss, "#"); break;
          case VariableType::Custom: appendFormatted(oss, "%"); break;
        }

        serializeUserVariableName(oss, variable.name);
      }

      void operator()(ConstantExpression const& constant) { serializeConstant(constant); }

      void operator()(ListExpression const& list)
      {
        appendFormatted(oss, "[");

        bool first = true;

        for (auto const& value : list.values)
        {
          if (!first)
          {
            appendFormatted(oss, ", ");
          }

          serializeConstant(value);
          first = false;
        }

        appendFormatted(oss, "]");
      }

      void operator()(RangeExpression const& range)
      {
        serializeConstant(range.lower);
        appendFormatted(oss, "..");
        serializeConstant(range.upper);
      }

      void serializeBinary(Operator op, Expression const& rhs)
      {
        // serializeBinary only ever receives binary (infix) operators, so the
        // canonical token from the table reproduces the original spacing exactly.
        appendFormatted(oss, " {} ", detail::operatorDescriptor(op).spelling);
        std::visit(*this, rhs);
      }

      void serializeConstant(ConstantExpression const& constant)
      {
        std::visit(
          utility::makeVisitor([this](bool val) { appendFormatted(oss, "{}", val ? "true" : "false"); },
                               [this](std::int64_t val) { appendFormatted(oss, "{}", val); },
                               [this](UnitConstantExpression const& val) { appendFormatted(oss, "{}", val.lexeme); },
                               [this](std::string_view val)
                               {
                                 appendFormatted(oss, "\"");
                                 appendEscapedString(oss, val);
                                 appendFormatted(oss, "\"");
                               }),
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
    return serializer.oss.str();
  }
} // namespace ao::query
