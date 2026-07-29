// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/rt/WritableTagList.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

namespace ao::rt::test
{
  TEST_CASE("WritableTagList - recognizes only a positive tag AST root", "[runtime][unit][writable-tag]")
  {
    CHECK(writableTagForListExpression(R"(#"road-trip")") == std::optional<std::string>{"road-trip"});
    CHECK(writableTagForListExpression(R"(#"Road Trip")") == std::optional<std::string>{"Road Trip"});
    CHECK(writableTagForListExpression(R"(  #"quote\"and\\slash"  )") ==
          std::optional<std::string>{R"(quote"and\slash)"});

    CHECK_FALSE(writableTagForListExpression("").has_value());
    CHECK_FALSE(writableTagForListExpression("#road-trip").has_value());
    CHECK_FALSE(writableTagForListExpression("$genre").has_value());
    CHECK_FALSE(writableTagForListExpression("not #blocked").has_value());
    CHECK_FALSE(writableTagForListExpression("#rock and $year >= 2020").has_value());
    CHECK_FALSE(writableTagForListExpression("#rock or #jazz").has_value());
    CHECK_FALSE(writableTagForListExpression("(").has_value());
  }

  TEST_CASE("WritableTagList - finds tag references anywhere in a valid expression", "[runtime][unit][writable-tag]")
  {
    CHECK(listExpressionReferencesTag(R"(#"road-trip")", "road-trip"));
    CHECK(listExpressionReferencesTag(R"((#rock or #jazz) and not #blocked)", "jazz"));
    CHECK(listExpressionReferencesTag(R"((#rock or #jazz) and not #blocked)", "blocked"));
    CHECK_FALSE(listExpressionReferencesTag(R"((#rock or #jazz) and not #blocked)", "ambient"));
    CHECK_FALSE(listExpressionReferencesTag("(", "rock"));
  }
} // namespace ao::rt::test
