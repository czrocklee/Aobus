// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ExecutionPlanTestSupport.h"

#include <ao/Error.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Expression.h>
#include <ao/query/Parser.h>
#include <ao/query/QueryCompilation.h>

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <utility>

namespace ao::query::test
{
  Expression parseOk(std::string_view const text)
  {
    auto res = ::ao::query::parse(text);
    REQUIRE(res.has_value());
    return std::move(*res);
  }

  ExecutionPlan compileOk(Expression const& expr)
  {
    auto res = compileQuery(expr);
    REQUIRE(res.has_value());
    return std::move(*res);
  }

  Error compileError(Expression const& expr)
  {
    auto res = compileQuery(expr);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().code == Error::Code::FormatRejected);
    return res.error();
  }
} // namespace ao::query::test
