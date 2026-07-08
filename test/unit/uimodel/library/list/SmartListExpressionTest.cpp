// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/library/list/SmartListExpression.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::test
{
  TEST_CASE("SmartListExpression - formats empty expression as none", "[uimodel][unit][list]")
  {
    CHECK(formatSmartListExpressionDisplayText("") == "(none)");
  }

  TEST_CASE("SmartListExpression - preserves non-empty display text", "[uimodel][unit][list]")
  {
    CHECK(formatSmartListExpressionDisplayText("$genre = 'Jazz'") == "$genre = 'Jazz'");
  }

  TEST_CASE("SmartListExpression - returns local expression without parent", "[uimodel][unit][list]")
  {
    CHECK(combineSmartListEffectiveExpression("", "$artist = 'Queen'") == "$artist = 'Queen'");
  }

  TEST_CASE("SmartListExpression - combines parent and local expressions", "[uimodel][unit][list]")
  {
    auto const effective = combineSmartListEffectiveExpression("$year > 1970", "$artist = 'Queen'");
    CHECK(effective == "($year > 1970) and ($artist = 'Queen')");
  }

  TEST_CASE("SmartListExpression - returns parent expression without local filter", "[uimodel][unit][list]")
  {
    CHECK(combineSmartListEffectiveExpression("$year > 1970", "") == "$year > 1970");
  }
} // namespace ao::uimodel::test
