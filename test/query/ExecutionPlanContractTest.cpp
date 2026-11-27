// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/query/ExecutionPlanTestSupport.h"
#include <ao/query/Field.h>
#include <ao/query/QueryCompilation.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::query::test
{
  TEST_CASE("compileQuery returns Result without throwing")
  {
    SECTION("Valid predicate yields a plan")
    {
      auto const planRes = compileQuery(parseOk("$year = 1990"));
      REQUIRE(planRes.has_value());
      CHECK(planRes->accessProfile != AccessProfile::NoTrackData);
    }

    SECTION("Non-predicate expression yields an Error")
    {
      auto const planRes = compileQuery(parseOk("$year"));
      REQUIRE_FALSE(planRes.has_value());
      CHECK(planRes.error().code == Error::Code::FormatRejected);
      CHECK_FALSE(planRes.error().message.empty());
    }
  }
} // namespace ao::query::test
