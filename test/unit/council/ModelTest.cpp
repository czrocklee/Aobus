// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "council/Model.h"

#include <catch2/catch_test_macros.hpp>

namespace ao::council::test
{
  TEST_CASE("Model - enum names round trip", "[council][unit][model]")
  {
    for (auto const& [value, name] : kFailureReasonNames)
    {
      CHECK(toString(value) == name);
      CHECK(enumFromName(kFailureReasonNames, name) == value);
    }

    for (auto const& [value, name] : kPromptDeliveryNames)
    {
      CHECK(toString(value) == name);
      CHECK(enumFromName(kPromptDeliveryNames, name) == value);
    }

    for (auto const& [value, name] : kDepthNames)
    {
      CHECK(toString(value) == name);
      CHECK(enumFromName(kDepthNames, name) == value);
    }

    for (auto const& [value, name] : kProcessStatusNames)
    {
      CHECK(toString(value) == name);
      CHECK(enumFromName(kProcessStatusNames, name) == value);
    }
  }

  TEST_CASE("Model - agent version uses effort only when present", "[council][unit][model]")
  {
    CHECK(AgentDefinition{.model = "model-a"}.modelVersion() == "model-a");
    CHECK(AgentDefinition{.model = "model-a", .effort = "high"}.modelVersion() == "model-a@high");
  }
} // namespace ao::council::test
