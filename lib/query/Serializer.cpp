// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/query/detail/OperatorTable.h>
#include <ao/utility/VariantVisitor.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
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

    void appendEscapedString(std::ostringstream& oss, std::string_view str)
    {
      for (auto const ch : str)
      {
        switch (ch)
        {
          case '\\': std::print(oss, "\\\\"); break;
          case '"': std::print(oss, "\\\""); break;
          case '\n': std::print(oss, "\\n"); break;
          case '\t': std::print(oss, "\\t"); break;
          case '\r': std::print(oss, "\\r"); break;
          default: std::print(oss, "{}", ch); break;
        }
      }
    }

    void serializeUserVariableName(std::ostringstream& oss, std::string_view name)
    {
      if (isSimpleUserVariableName(name))
      {
        std::print(oss, "{}", name);
        return;
      }

      std::print(oss, "\"");
      appendEscapedString(oss, name);
      std::print(oss, "\"");
    }

    struct [[nodiscard]] ParenthesisGuard final
    {
      ParenthesisGuard(std::ostringstream& oss, bool apply)
        : oss{oss}, apply{apply}
      {
        if (apply)
        {
          oss.put('(');
        }
      }

      ParenthesisGuard(ParenthesisGuard const&) = delete;
      ParenthesisGuard& operator=(ParenthesisGuard const&) = delete;
      ParenthesisGuard(ParenthesisGuard&&) = delete;
      ParenthesisGuard& operator=(ParenthesisGuard&&) = delete;

      ~ParenthesisGuard()
      {
        if (apply)
        {
          oss.put(')');
        }
      }

      std::ostringstream& oss;
      bool apply;
    };

    struct Serializer final
    {
      Serializer() = default;

      void operator()(std::unique_ptr<BinaryExpression> const& binary)
      {
        if (!binary)
        {
          return;
        }

        auto guard = ParenthesisGuard{oss, (counter++ > 0) && binary->optOperation};
        std::visit(*this, binary->operand);

        if (binary->optOperation)
        {
          serializeBinary(binary->optOperation->op, binary->optOperation->operand);
        }
      }

      void operator()(std::unique_ptr<UnaryExpression> const& unary)
      {
        if (!unary)
        {
          return;
        }

        if (unary->op == Operator::Exists)
        {
          auto const needsParens = std::get_if<VariableExpression>(&unary->operand) == nullptr;
          {
            auto guard = ParenthesisGuard{oss, needsParens};
            std::visit(*this, unary->operand);
          }
          std::print(oss, "?");
          return;
        }

        std::print(oss, "not ");
        std::visit(*this, unary->operand);
      }

      void operator()(VariableExpression const& variable)
      {
        switch (variable.type)
        {
          case VariableType::Metadata: std::print(oss, "${}", variable.name); return;
          case VariableType::Property: std::print(oss, "@{}", variable.name); return;
          case VariableType::Tag: std::print(oss, "#"); break;
          case VariableType::Custom: std::print(oss, "%"); break;
        }

        serializeUserVariableName(oss, variable.name);
      }

      void operator()(ConstantExpression const& constant) { serializeConstant(constant); }

      void operator()(ListExpression const& list)
      {
        std::print(oss, "[");

        bool first = true;

        for (auto const& value : list.values)
        {
          if (!first)
          {
            std::print(oss, ", ");
          }

          serializeConstant(value);
          first = false;
        }

        std::print(oss, "]");
      }

      void operator()(RangeExpression const& range)
      {
        serializeConstant(range.lower);
        std::print(oss, "..");
        serializeConstant(range.upper);
      }

      void serializeBinary(Operator op, Expression const& rhs)
      {
        // serializeBinary only ever receives binary (infix) operators, so the
        // canonical token from the table reproduces the original spacing exactly.
        std::print(oss, " {} ", detail::operatorInfo(op).spelling);
        std::visit(*this, rhs);
      }

      void serializeConstant(ConstantExpression const& constant)
      {
        std::visit(
          utility::makeVisitor([this](bool val) { std::print(oss, "{}", val ? "true" : "false"); },
                               [this](std::int64_t val) { std::print(oss, "{}", val); },
                               [this](UnitConstantExpression const& val) { std::print(oss, "{}", val.lexeme); },
                               [this](std::string_view val)
                               {
                                 std::print(oss, "\"");
                                 appendEscapedString(oss, val);
                                 std::print(oss, "\"");
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
