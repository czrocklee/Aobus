// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/query/Expression.h>
#include <ao/query/Parser.h>
#include <ao/rt/WritableTagList.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace ao::rt
{
  namespace
  {
    bool expressionReferencesTag(query::Expression const& expression, std::string_view const tag)
    {
      if (auto const* variable = std::get_if<query::VariableExpression>(&expression); variable != nullptr)
      {
        return variable->type == query::VariableType::Tag && variable->name == tag;
      }

      if (auto const* binary = std::get_if<std::unique_ptr<query::BinaryExpression>>(&expression);
          binary != nullptr && *binary != nullptr)
      {
        return expressionReferencesTag((*binary)->operand, tag) ||
               ((*binary)->optOperation && expressionReferencesTag((*binary)->optOperation->operand, tag));
      }

      if (auto const* unary = std::get_if<std::unique_ptr<query::UnaryExpression>>(&expression);
          unary != nullptr && *unary != nullptr)
      {
        return expressionReferencesTag((*unary)->operand, tag);
      }

      return false;
    }
  } // namespace

  std::optional<std::string> writableTagForListExpression(std::string_view const expression)
  {
    auto parsed = query::parse(expression);

    if (!parsed)
    {
      return std::nullopt;
    }

    auto const* const variable = std::get_if<query::VariableExpression>(&*parsed);

    if (variable == nullptr || variable->type != query::VariableType::Tag)
    {
      return std::nullopt;
    }

    return variable->name;
  }

  bool listExpressionReferencesTag(std::string_view const expression, std::string_view const tag)
  {
    auto parsed = query::parse(expression);
    return parsed && expressionReferencesTag(*parsed, tag);
  }
} // namespace ao::rt
