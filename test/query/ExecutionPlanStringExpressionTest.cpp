// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/query/ExecutionPlanTestSupport.h"
#include <ao/Error.h>
#include <ao/query/Field.h>
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <tuple>
#include <vector>

namespace ao::query::test
{
  namespace
  {
    InstructionIterator checkLogical(ExecutionPlan const& plan,
                                     OpCode op,
                                     InstructionIterator left,
                                     InstructionIterator right,
                                     std::size_t occurrence = 0)
    {
      auto const logical = findInstruction(plan, op, occurrence);
      CHECK(left < right);
      CHECK(right < logical);
      CHECK(right->operand == left->operand + 1);
      CHECK(logical->operand == right->operand - 1);
      return logical;
    }
  } // namespace

  TEST_CASE("ExecutionPlan - compiles text substring operators as Unicode caseless")
  {
    auto expr = parseOk("$title ~ Love");
    auto plan = compileOk(expr);

    CHECK(plan.stringConstants == std::vector<std::string>{"love"});
    CHECK(plan.dictionarySymbols.empty());
    CHECK_FALSE(plan.requiresDictionary);
    CHECK(checkComparison(plan, OpCode::Like, Field::Title, 0)->operand == 1);
  }

  TEST_CASE("ExecutionPlan - compiles Unicode caseless substring keys")
  {
    auto const plan = compileOk(parseOk("$title ~ 'STRASSE Cafe\u0301'"));

    CHECK(plan.stringConstants == std::vector<std::string>{"strasse café"});
    CHECK(checkComparison(plan, OpCode::Like, Field::Title, 0)->operand == 1);

    auto const expandedPlan = compileOk(parseOk("$title ~ 'Straße'"));
    CHECK(expandedPlan.stringConstants == std::vector<std::string>{"strasse"});
    CHECK(checkComparison(expandedPlan, OpCode::Like, Field::Title, 0)->operand == 1);
  }

  TEST_CASE("ExecutionPlan - validates substring operands")
  {
    CHECK(compileError(parseOk("$title ~ 123")).code == Error::Code::FormatRejected);
    CHECK(compileError(parseOk("@duration ~ 'three minutes'")).code == Error::Code::FormatRejected);
    CHECK(compileError(parseOk("$coverArt ~ 'front'")).code == Error::Code::FormatRejected);
    CHECK(compileError(parseOk("#rock ~ 'progressive'")).code == Error::Code::FormatRejected);
  }

  TEST_CASE("ExecutionPlan - compiles string constants")
  {
    auto expr = parseOk("$title = 'Hello World'");
    auto plan = compileOk(expr);

    CHECK(plan.stringConstants == std::vector<std::string>{"Hello World"});
  }

  TEST_CASE("ExecutionPlan - compiles LIKE for album ids")
  {
    auto expr = parseOk(R"($album ~ "Greatest Hits")");
    auto plan = compileOk(expr);

    CHECK(plan.stringConstants == std::vector<std::string>{"greatest hits"});
    CHECK(plan.dictionarySymbols.empty());
    CHECK(plan.requiresDictionary);
    CHECK(checkComparison(plan, OpCode::Like, Field::AlbumId, 0)->operand == 1);
  }

  TEST_CASE("ExecutionPlan - compiles LIKE for genre ids")
  {
    auto expr = parseOk(R"($genre ~ "Rock")");
    auto plan = compileOk(expr);

    CHECK(plan.stringConstants == std::vector<std::string>{"rock"});
    CHECK(plan.dictionarySymbols.empty());
    CHECK(plan.requiresDictionary);
    CHECK(checkComparison(plan, OpCode::Like, Field::GenreId, 0)->operand == 1);
  }

  TEST_CASE("ExecutionPlan - compiles LIKE for album artist ids")
  {
    auto expr = parseOk(R"($albumArtist ~ "Bach")");
    auto plan = compileOk(expr);

    CHECK(plan.stringConstants == std::vector<std::string>{"bach"});
    CHECK(plan.dictionarySymbols.empty());
    CHECK(plan.requiresDictionary);
    CHECK(checkComparison(plan, OpCode::Like, Field::AlbumArtistId, 0)->operand == 1);
  }

  TEST_CASE("ExecutionPlan - rejects LIKE for cover art ids")
  {
    auto expr = parseOk(R"($coverArt ~ "front")");
    std::ignore = compileError(expr);
  }

  TEST_CASE("ExecutionPlan - rejects LIKE for tags")
  {
    auto expr = parseOk(R"(#rock ~ "progressive")");
    std::ignore = compileError(expr);
  }

  TEST_CASE("ExecutionPlan - compiles LIKE for titles")
  {
    auto expr = parseOk(R"($title ~ "Bach")");
    auto plan = compileOk(expr);

    CHECK(plan.stringConstants == std::vector<std::string>{"bach"});
    CHECK(plan.dictionarySymbols.empty());
    CHECK_FALSE(plan.requiresDictionary);
    CHECK(checkComparison(plan, OpCode::Like, Field::Title, 0)->operand == 1);
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles mixed LIKE and EQUAL in OR expressions")
  {
    // This tests that leftField is correctly saved before compiling right operand
    // $title ~ "Bach" should NOT check if ArtistId is used with LIKE
    auto expr = parseOk(R"($title ~ "Bach" or $artist = "Bach")");
    auto plan = compileOk(expr);

    CHECK(plan.stringConstants == std::vector<std::string>{"bach"});
    CHECK(plan.dictionarySymbols == std::vector<std::string>{"Bach"});
    CHECK(plan.requiresDictionary);
    auto const title = checkComparison(plan, OpCode::Like, Field::Title, 0);
    auto const artist = checkComparison(plan, OpCode::Eq, Field::ArtistId, 0, 0);
    CHECK(checkLogical(plan, OpCode::Or, title, artist)->operand == 1);
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles parenthesized LIKE and EQUAL in OR expressions")
  {
    // Explicit grouping with parentheses should also work
    auto expr = parseOk(R"(($title ~ "Bach") or ($artist = "Bach"))");
    auto plan = compileOk(expr);

    CHECK(plan.stringConstants == std::vector<std::string>{"bach"});
    CHECK(plan.dictionarySymbols == std::vector<std::string>{"Bach"});
    CHECK(plan.requiresDictionary);
    auto const title = checkComparison(plan, OpCode::Like, Field::Title, 0);
    auto const artist = checkComparison(plan, OpCode::Eq, Field::ArtistId, 0, 0);
    CHECK(checkLogical(plan, OpCode::Or, title, artist)->operand == 1);
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles multiple OR branches with ID field equality")
  {
    // Each equality must retain its own field and bindable symbol.
    auto expr = parseOk(R"($artist = "Bach" or $artist = "Mozart" or $album = "交响乐")");
    auto plan = compileOk(expr);

    CHECK(plan.dictionarySymbols == std::vector<std::string>{"Bach", "Mozart", "交响乐"});
    CHECK(plan.stringConstants.empty());
    CHECK(plan.requiresDictionary);
    auto const bach = checkComparison(plan, OpCode::Eq, Field::ArtistId, 0, 0);
    auto const mozart = checkComparison(plan, OpCode::Eq, Field::ArtistId, 0, 1, 1);
    auto const album = checkComparison(plan, OpCode::Eq, Field::AlbumId, 0, 2, 2);
    auto const innerOr = checkLogical(plan, OpCode::Or, mozart, album);
    CHECK(checkLogical(plan, OpCode::Or, bach, innerOr, 1)->operand == 1);
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles title LIKE chained with AND")
  {
    // Title LIKE should work with AND
    auto expr = parseOk(R"($title ~ "Bach" and $year > 2000)");
    auto plan = compileOk(expr);

    CHECK(plan.stringConstants == std::vector<std::string>{"bach"});
    CHECK(plan.dictionarySymbols.empty());
    auto const title = checkComparison(plan, OpCode::Like, Field::Title, 0);
    auto const year = checkComparison(plan, OpCode::Gt, Field::Year, 2000);
    CHECK(checkLogical(plan, OpCode::And, title, year)->operand == 1);
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - deduplicates string constants")
  {
    SECTION("Reuses Identical String Constants")
    {
      auto expr = parseOk(R"($title = "Bach" or $title != "Bach")");
      auto plan = compileOk(expr);
      CHECK(plan.stringConstants == std::vector<std::string>{"Bach"});
    }

    SECTION("Stores Different String Constants Separately")
    {
      auto expr = parseOk(R"($title = "Bach" or $title = "Mozart")");
      auto plan = compileOk(expr);
      CHECK(plan.stringConstants == std::vector<std::string>{"Bach", "Mozart"});
    }
  }
} // namespace ao::query::test
