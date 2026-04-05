// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/expr/Serializer.h>
#include <rs/utility/VariantVisitor.h>

#include <sstream>

namespace
{
  using namespace rs::expr;

  struct ParenthesisGuard // NOLINT(cppcoreguidelines-special-member-functions)
  {
    ParenthesisGuard(std::ostringstream& oss, bool apply)
      : oss{oss}, apply{apply}
    {
      if (apply) { oss << "("; }
    }

    ~ParenthesisGuard()
    {
      if (apply) { oss << ")"; }
    }
    
    std::ostringstream& oss;
    bool apply;
  };

  struct Serializer
  {
    Serializer() {};

    void operator()(std::unique_ptr<BinaryExpression> const& binary)
    {
      if (!binary) { return; }
      auto guard = ParenthesisGuard{oss, (counter++ > 0) && binary->operation};
      std::visit(*this, binary->operand);

      if (binary->operation) { serializeBinary(binary->operation->op, binary->operation->operand); }
    }

    void operator()(std::unique_ptr<UnaryExpression> const& unary)
    {
      if (!unary) { return; }
      oss << "not ";
      std::visit(*this, unary->operand);
    }

    void operator()(VariableExpression const& variable)
    {
      switch (variable.type)
      {
        case VariableType::Metadata: oss << '$'; break;
        case VariableType::Property: oss << '@'; break;
        case VariableType::Tag: oss << '#'; break;
        case VariableType::Custom: oss << '%'; break;
      }

      oss << variable.name;
    }

    void operator()(ConstantExpression const& constant)
    {
      std::visit(rs::utility::makeVisitor([](std::monostate) {},
                                          [this](bool val) { oss << (val ? "true" : "false"); },
                                          [this](std::int64_t val) { oss << val; },
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

namespace rs::expr
{
  std::string serialize(Expression const& expr)
  {
    auto serializer = Serializer{};
    std::visit(serializer, expr);
    return serializer.oss.str();
  }
}
