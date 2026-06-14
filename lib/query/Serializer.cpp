// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/utility/VariantVisitor.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
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

    void serializeUserVariableName(std::ostringstream& oss, std::string_view name)
    {
      if (isSimpleUserVariableName(name))
      {
        oss << name;
        return;
      }

      oss << '"';

      for (auto const ch : name)
      {
        if (ch == '"' || ch == '\\')
        {
          oss << '\\';
        }

        oss << ch;
      }

      oss << '"';
    }

    struct [[nodiscard]] ParenthesisGuard final
    {
      ParenthesisGuard(std::ostringstream& oss, bool apply)
        : oss{oss}, apply{apply}
      {
        if (apply)
        {
          oss << "(";
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
          oss << ")";
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

        oss << "not ";
        std::visit(*this, unary->operand);
      }

      void operator()(VariableExpression const& variable)
      {
        switch (variable.type)
        {
          case VariableType::Metadata: oss << '$' << variable.name; return;
          case VariableType::Property: oss << '@' << variable.name; return;
          case VariableType::Tag: oss << '#'; break;
          case VariableType::Custom: oss << '%'; break;
        }

        serializeUserVariableName(oss, variable.name);
      }

      void operator()(ConstantExpression const& constant)
      {
        std::visit(utility::makeVisitor([](std::monostate) {},
                                        [this](bool val) { oss << (val ? "true" : "false"); },
                                        [this](std::int64_t val) { oss << val; },
                                        [this](UnitConstantExpression const& val) { oss << val.lexeme; },
                                        [this](std::string_view val) { oss << "\"" << val << "\""; }),
                   constant);
      }

      void serializeBinary(Operator op, Expression const& rhs)
      {
        switch (op)
        {
          case Operator::And: oss << " and "; break;
          case Operator::Or: oss << " or "; break;
          case Operator::Less: oss << " < "; break;
          case Operator::LessEqual: oss << " <= "; break;
          case Operator::Greater: oss << " > "; break;
          case Operator::GreaterEqual: oss << " >= "; break;
          case Operator::Equal: oss << " = "; break;
          case Operator::NotEqual: oss << " != "; break;
          case Operator::Like: oss << " ~ "; break;
          case Operator::Add: oss << " + "; break;
          default: break;
        }

        std::visit(*this, rhs);
      }

      std::ostringstream oss;
      std::size_t counter = 0;
    };
  }

  std::string serialize(Expression const& expr)
  {
    auto serializer = Serializer{};
    std::visit(serializer, expr);
    return serializer.oss.str();
  }
}
