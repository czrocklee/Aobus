// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ExecutionPlanTestSupport.h"

#include <ao/Error.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Expression.h>
#include <ao/query/Parser.h>
#include <ao/query/QueryCompiler.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <utility>

namespace ao::query::test
{
  Expression parseOk(std::string_view const text)
  {
    auto result = ::ao::query::parse(text);
    REQUIRE(result.has_value());
    return std::move(*result);
  }

  ExecutionPlan compileOk(QueryCompiler& compiler, Expression const& expr)
  {
    auto result = compiler.compile(expr);
    REQUIRE(result.has_value());
    return std::move(*result);
  }

  ExecutionPlan compileOk(QueryCompiler&& compiler, Expression const& expr)
  {
    auto local = std::move(compiler);
    return compileOk(local, expr);
  }

  Error compileError(QueryCompiler& compiler, Expression const& expr)
  {
    auto result = compiler.compile(expr);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Error::Code::FormatRejected);
    return result.error();
  }
} // namespace ao::query::test
