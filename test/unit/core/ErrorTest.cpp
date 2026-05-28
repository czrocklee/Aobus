// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/Error.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace ao::test
{
  TEST_CASE("Error - Recoverable error infrastructure", "[core][unit][error]")
  {
    SECTION("makeError produces correct Error payload")
    {
      auto const result = Result{makeError(Error::Code::NotFound, "Item not in database")};

      REQUIRE_FALSE(result.has_value());
      CHECK(result.error().code == Error::Code::NotFound);
      CHECK(result.error().message == "Item not in database");
    }

    SECTION("Result<T> integration with makeError")
    {
      auto fn = [] -> Result<int> { return makeError(Error::Code::InvalidState, "Bad state"); };

      auto const res = fn();
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidState);
      CHECK(res.error().message == "Bad state");
    }
  }
} // namespace ao::test
