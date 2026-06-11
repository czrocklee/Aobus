// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "fleet/Model.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::fleet::test
{
  TEST_CASE("Fleet model - enum name tables drive protocol serialization", "[fleet][unit][model]")
  {
    SECTION("failure reasons use their protocol names")
    {
      for (auto const& [value, name] : kFailureReasonNames)
      {
        CHECK(toString(value) == name);
      }
    }

    SECTION("oracle runners use their registry names")
    {
      for (auto const& [value, name] : kOracleRunnerNames)
      {
        CHECK(toString(value) == name);
      }
    }

    SECTION("escalation actions use their manifest names")
    {
      for (auto const& [value, name] : kEscalationActionNames)
      {
        CHECK(toString(value) == name);
      }
    }
  }
} // namespace ao::fleet::test
