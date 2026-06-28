// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestUtils.h"
#include <ao/AudioCodec.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

namespace ao::query::test
{
  TEST_CASE("ExecutionPlan - compiles simple expressions", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("$artist = Bach");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles constant true expressions", "[query][unit][execution_plan]")
  {
    // Note: matchesAll is not automatically set - it's a hint for optimization
    // The plan should still compile a constant true expression
    auto expr = parseOk("true");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    // The plan should have at least one instruction (LoadConstant)
    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - compiles metadata fields", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("$title = 'Test'");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.instructions.size() >= 2);
    CHECK(plan.instructions[0].op == OpCode::LoadField);

    bool hasEq = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::Eq)
      {
        hasEq = true;
        break;
      }
    }

    CHECK(hasEq == true);
  }

  TEST_CASE("ExecutionPlan - compiles property fields", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("@duration > 180000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.instructions.size() >= 2);
    CHECK(plan.instructions[0].op == OpCode::LoadField);

    bool hasGt = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::Gt)
      {
        hasGt = true;
        break;
      }
    }

    CHECK(hasGt == true);
  }

  TEST_CASE("ExecutionPlan - maps property aliases to bitrate fields", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("@br >= 320k");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    REQUIRE_FALSE(plan.instructions.empty());
    CHECK(plan.instructions[0].op == OpCode::LoadField);
    CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::Bitrate));
  }

  TEST_CASE("ExecutionPlan - maps metadata aliases to album artist fields", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("$aa = Bach");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    REQUIRE_FALSE(plan.instructions.empty());
    CHECK(plan.instructions[0].op == OpCode::LoadField);
    CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::AlbumArtistId));
  }

  TEST_CASE("ExecutionPlan - compiles codec constants", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, parseOk("@codec = AAC"));

    auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

    REQUIRE(it != plan.instructions.end());
    CHECK(std::cmp_equal(it->constValue, audioCodecStorageValue(AudioCodec::Aac)));
  }

  TEST_CASE("ExecutionPlan - rejects unsupported codec constants", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};

    std::ignore = compileError(compiler, parseOk("@codec = OPUS"));
  }

  TEST_CASE("ExecutionPlan - compiles logical and", "[query][unit][execution_plan]")
  {
    // Use && for logical and to ensure it's parsed correctly
    auto expr = parseOk("$artist = Bach && $genre = Classical");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    bool hasAnd = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::And)
      {
        hasAnd = true;
        break;
      }
    }

    CHECK(hasAnd == true);
  }

  TEST_CASE("ExecutionPlan - compiles logical or", "[query][unit][execution_plan]")
  {
    // Use || for logical or to ensure it's parsed correctly
    auto expr = parseOk("$artist = Bach || $artist = Mozart");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    bool hasOr = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::Or)
      {
        hasOr = true;
        break;
      }
    }

    CHECK(hasOr == true);
  }

  TEST_CASE("ExecutionPlan - compiles in lists", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};

    SECTION("CompilesConstantListAsMembershipSet")
    {
      auto const plan = compileOk(compiler, parseOk("$year in [1990, 1991, 1992]"));

      REQUIRE(plan.inSets.size() == 1);
      CHECK(plan.inSets[0].numericValues.contains(1991));
      CHECK(plan.inSets[0].numericValues.contains(2000) == false);
      CHECK(std::ranges::count(plan.instructions, OpCode::InSet, &Instruction::op) == 1);
      CHECK(std::ranges::count(plan.instructions, OpCode::Eq, &Instruction::op) == 0);
      CHECK(std::ranges::count(plan.instructions, OpCode::Or, &Instruction::op) == 0);
    }

    SECTION("CompilesSingleItemListAsMembershipSet")
    {
      auto const plan = compileOk(compiler, parseOk("$year in [1990]"));

      REQUIRE(plan.inSets.size() == 1);
      CHECK(plan.inSets[0].numericValues.contains(1990));
      CHECK(std::ranges::count(plan.instructions, OpCode::InSet, &Instruction::op) == 1);
      CHECK(std::ranges::count(plan.instructions, OpCode::Eq, &Instruction::op) == 0);
      CHECK(std::ranges::count(plan.instructions, OpCode::Or, &Instruction::op) == 0);
    }

    SECTION("RejectsStandaloneList")
    {
      std::ignore = compileError(compiler, parseOk("[1990, 1991]"));
    }

    SECTION("RejectsNonListRightOperand")
    {
      std::ignore = compileError(compiler, parseOk("$artist in Bach"));
    }
  }

  TEST_CASE("ExecutionPlan - compiles in ranges", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};

    SECTION("CompilesRangeAsClosedBounds")
    {
      auto const plan = compileOk(compiler, parseOk("$year in 1990..1999"));

      CHECK(std::ranges::count(plan.instructions, OpCode::Ge, &Instruction::op) == 1);
      CHECK(std::ranges::count(plan.instructions, OpCode::Le, &Instruction::op) == 1);
      CHECK(std::ranges::count(plan.instructions, OpCode::And, &Instruction::op) == 1);
    }

    SECTION("ScalesDurationRangeBounds")
    {
      auto const plan = compileOk(compiler, parseOk("@duration in 2m30s..5m"));

      REQUIRE(plan.instructions.size() >= 5);
      CHECK(plan.instructions[1].op == OpCode::LoadConstant);
      CHECK(plan.instructions[1].constValue == 150000);
      CHECK(plan.instructions[4].op == OpCode::LoadConstant);
      CHECK(plan.instructions[4].constValue == 300000);
    }

    SECTION("RejectsStandaloneRange")
    {
      std::ignore = compileError(compiler, parseOk("1990..1999"));
    }

    SECTION("CompilesDictionaryRangeAsStringBounds")
    {
      // Dictionary fields hold interned IDs, so range bounds are kept as string
      // constants (not resolved to IDs) for lexicographic comparison at eval time.
      auto const plan = compileOk(compiler, parseOk("$artist in Bach..Mozart"));

      REQUIRE(plan.stringConstants.size() == 2);
      CHECK(plan.stringConstants[0] == "Bach");
      CHECK(plan.stringConstants[1] == "Mozart");
      CHECK(std::ranges::count(plan.instructions, OpCode::Ge, &Instruction::op) == 1);
      CHECK(std::ranges::count(plan.instructions, OpCode::Le, &Instruction::op) == 1);
    }

    SECTION("RejectsNonStringDictionaryRangeBounds")
    {
      // An ordered comparison over a dictionary field only makes sense against text.
      std::ignore = compileError(compiler, parseOk("$artist in 1..5"));
    }

    SECTION("AllowsRangeOnStringField")
    {
      // Plain string fields compare lexicographically, so a range is meaningful.
      std::ignore = compileOk(compiler, parseOk("$title in apple..zoo"));
    }
  }

  TEST_CASE("ExecutionPlan - enforces ordered comparison field restrictions", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};

    SECTION("AllowsStringRelationalOnDictionaryField")
    {
      // Ordered comparisons over dictionary fields compare resolved text; the
      // operand is kept as a string constant rather than resolved to an ID.
      auto const plan = compileOk(compiler, parseOk("$artist > Bach"));

      REQUIRE(plan.stringConstants.size() == 1);
      CHECK(plan.stringConstants[0] == "Bach");
      CHECK(std::ranges::count(plan.instructions, OpCode::Gt, &Instruction::op) == 1);
    }

    SECTION("RejectsNonStringRelationalOnDictionaryField")
    {
      std::ignore = compileError(compiler, parseOk("$artist > 5"));
      std::ignore = compileError(compiler, parseOk("$genre <= 3m"));
    }

    SECTION("AllowsEqualityOnDictionaryField")
    {
      std::ignore = compileOk(compiler, parseOk("$artist = Bach"));
      std::ignore = compileOk(compiler, parseOk("$genre in [Classical, Jazz]"));
    }

    SECTION("AllowsRelationalOnNumericAndStringFields")
    {
      std::ignore = compileOk(compiler, parseOk("$year < 1990"));
      std::ignore = compileOk(compiler, parseOk("$title < zoo"));
      std::ignore = compileOk(compiler, parseOk("$coverArt > 0"));
    }
  }

  TEST_CASE("ExecutionPlan - compiles logical not", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("not #favorite");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    bool hasNot = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::Not)
      {
        hasNot = true;
        break;
      }
    }

    CHECK(hasNot == true);
  }

  TEST_CASE("ExecutionPlan - compiles existence tests", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};

    SECTION("FieldExistenceEmitsExistsOpcode")
    {
      auto const plan = compileOk(compiler, parseOk("$year?"));

      REQUIRE(plan.instructions.size() == 1);
      CHECK(plan.instructions[0].op == OpCode::Exists);
      CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::Year));
      CHECK(plan.accessProfile == AccessProfile::HotOnly);
    }

    SECTION("ColdFieldExistenceUpdatesAccessProfile")
    {
      auto const plan = compileOk(compiler, parseOk("@duration?"));

      REQUIRE(plan.instructions.size() == 1);
      CHECK(plan.instructions[0].op == OpCode::Exists);
      CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::Duration));
      CHECK(plan.accessProfile == AccessProfile::ColdOnly);
    }

    SECTION("BareNonTagVariablesAreRejectedAsPredicates")
    {
      std::ignore = compileError(compiler, parseOk("$year"));
      std::ignore = compileError(compiler, parseOk("@duration"));
      std::ignore = compileError(compiler, parseOk("%rating"));
      std::ignore = compileError(compiler, parseOk("not $year"));
      CHECK_THAT(compileError(compiler, parseOk("!$year")).message, Catch::Matchers::ContainsSubstring("!$year?"));
      std::ignore = compileError(compiler, parseOk("$artist and $year = 1990"));
      std::ignore = compileError(compiler, parseOk("$artist or $year = 1990"));
      std::ignore = compileError(compiler, parseOk("$year = 1990 or $artist"));
    }

    SECTION("ExistenceRequiresVariableOperand")
    {
      std::ignore = compileError(compiler, parseOk("($year = 1990)?"));
      std::ignore = compileError(compiler, parseOk("1990?"));
      std::ignore = compileError(compiler, parseOk(R"("Bach"?)"));
    }

    SECTION("BareTagsRemainPredicates")
    {
      std::ignore = compileOk(compiler, parseOk("#favorite"));
      std::ignore = compileOk(compiler, parseOk("!#favorite"));
    }
  }

  TEST_CASE("ExecutionPlan - compiles relational operators", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("$year < 2000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    bool hasLt = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::Lt)
      {
        hasLt = true;
        break;
      }
    }

    CHECK(hasLt == true);

    expr = parseOk("$year <= 2000");
    compiler = QueryCompiler{};
    plan = compileOk(compiler, expr);

    bool hasLe = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::Le)
      {
        hasLe = true;
        break;
      }
    }

    CHECK(hasLe == true);
  }

  TEST_CASE("ExecutionPlan - compiles like operators", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("$title ~ Love");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    bool hasLike = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::Like)
      {
        hasLike = true;
        break;
      }
    }

    CHECK(hasLike == true);
  }

  TEST_CASE("ExecutionPlan - compiles string constants", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("$title = 'Hello World'");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.stringConstants.empty());
    CHECK(plan.stringConstants[0] == "Hello World");
  }

  TEST_CASE("ExecutionPlan - compiles LIKE for album ids", "[query][unit][execution_plan]")
  {
    auto expr = parseOk(R"($album ~ "Greatest Hits")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - compiles LIKE for genre ids", "[query][unit][execution_plan]")
  {
    auto expr = parseOk(R"($genre ~ "Rock")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - compiles LIKE for album artist ids", "[query][unit][execution_plan]")
  {
    auto expr = parseOk(R"($albumArtist ~ "Bach")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - rejects LIKE for cover art ids", "[query][unit][execution_plan]")
  {
    auto expr = parseOk(R"($coverArt ~ "front")");
    auto compiler = QueryCompiler{};
    std::ignore = compileError(compiler, expr);
  }

  TEST_CASE("ExecutionPlan - rejects LIKE for tags", "[query][unit][execution_plan]")
  {
    auto expr = parseOk(R"(#rock ~ "progressive")");
    auto compiler = QueryCompiler{};
    std::ignore = compileError(compiler, expr);
  }

  TEST_CASE("ExecutionPlan - compiles LIKE for titles", "[query][unit][execution_plan]")
  {
    auto expr = parseOk(R"($title ~ "Bach")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - rejects unknown metadata fields", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("$uri = 'x'");
    auto compiler = QueryCompiler{};
    std::ignore = compileError(compiler, expr);
  }

  TEST_CASE("ExecutionPlan - rejects unknown property fields", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("@tagCount > 0");
    auto compiler = QueryCompiler{};
    std::ignore = compileError(compiler, expr);
  }

  TEST_CASE("ExecutionPlan - rejects add operators", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("$title + $artist");
    auto compiler = QueryCompiler{};
    std::ignore = compileError(compiler, expr);
  }

  TEST_CASE("ExecutionPlan - compiles mixed LIKE and EQUAL in OR expressions", "[query][unit][execution_plan]")
  {
    // This tests that leftField is correctly saved before compiling right operand
    // $title ~ "Bach" should NOT check if ArtistId is used with LIKE
    auto expr = parseOk(R"($title ~ "Bach" or $artist = "Bach")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles parenthesized LIKE and EQUAL in OR expressions", "[query][unit][execution_plan]")
  {
    // Explicit grouping with parentheses should also work
    auto expr = parseOk(R"(($title ~ "Bach") or ($artist = "Bach"))");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles multiple OR branches with ID field equality", "[query][unit][execution_plan]")
  {
    // Multiple ID field equalities in OR should compile without throwing
    auto expr = parseOk(R"($artist = "Bach" or $artist = "Mozart" or $album = "交响乐")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - compiles title LIKE chained with AND", "[query][unit][execution_plan]")
  {
    // Title LIKE should work with AND
    auto expr = parseOk(R"($title ~ "Bach" and $year > 2000)");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - maps every supported metadata catalog name", "[query][unit][execution_plan]")
  {
    struct Case final
    {
      std::string name;
      Field expected;
    };
    auto cases = {Case{.name = "year", .expected = Field::Year},
                  Case{.name = "y", .expected = Field::Year},
                  Case{.name = "trackNumber", .expected = Field::TrackNumber},
                  Case{.name = "tn", .expected = Field::TrackNumber},
                  Case{.name = "trackTotal", .expected = Field::TrackTotal},
                  Case{.name = "tt", .expected = Field::TrackTotal},
                  Case{.name = "discNumber", .expected = Field::DiscNumber},
                  Case{.name = "dn", .expected = Field::DiscNumber},
                  Case{.name = "discTotal", .expected = Field::DiscTotal},
                  Case{.name = "td", .expected = Field::DiscTotal},
                  Case{.name = "artist", .expected = Field::ArtistId},
                  Case{.name = "a", .expected = Field::ArtistId},
                  Case{.name = "album", .expected = Field::AlbumId},
                  Case{.name = "al", .expected = Field::AlbumId},
                  Case{.name = "genre", .expected = Field::GenreId},
                  Case{.name = "g", .expected = Field::GenreId},
                  Case{.name = "composer", .expected = Field::ComposerId},
                  Case{.name = "c", .expected = Field::ComposerId},
                  Case{.name = "albumArtist", .expected = Field::AlbumArtistId},
                  Case{.name = "aa", .expected = Field::AlbumArtistId},
                  Case{.name = "coverArt", .expected = Field::CoverArtId},
                  Case{.name = "ca", .expected = Field::CoverArtId},
                  Case{.name = "title", .expected = Field::Title},
                  Case{.name = "t", .expected = Field::Title},
                  Case{.name = "work", .expected = Field::WorkId},
                  Case{.name = "w", .expected = Field::WorkId}};

    for (auto const& c : cases)
    {
      DYNAMIC_SECTION("Field: " << c.name)
      {
        auto expr = parseOk("$" + c.name + " = 'x'");
        auto compiler = QueryCompiler{};
        auto plan = compileOk(compiler, expr);
        REQUIRE_FALSE(plan.instructions.empty());
        CHECK(plan.instructions[0].op == OpCode::LoadField);
        CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(c.expected));
      }
    }
  }

  TEST_CASE("ExecutionPlan - maps every supported property catalog name", "[query][unit][execution_plan]")
  {
    struct Case final
    {
      std::string name;
      Field expected;
    };
    auto cases = {Case{.name = "duration", .expected = Field::Duration},
                  Case{.name = "l", .expected = Field::Duration},
                  Case{.name = "bitrate", .expected = Field::Bitrate},
                  Case{.name = "br", .expected = Field::Bitrate},
                  Case{.name = "sampleRate", .expected = Field::SampleRate},
                  Case{.name = "sr", .expected = Field::SampleRate},
                  Case{.name = "channels", .expected = Field::Channels},
                  Case{.name = "bitDepth", .expected = Field::BitDepth},
                  Case{.name = "bd", .expected = Field::BitDepth},
                  Case{.name = "codec", .expected = Field::Codec}};

    for (auto const& c : cases)
    {
      DYNAMIC_SECTION("Field: " << c.name)
      {
        auto expr = parseOk("@" + c.name + " >= 0");
        auto compiler = QueryCompiler{};
        auto plan = compileOk(compiler, expr);
        REQUIRE_FALSE(plan.instructions.empty());
        CHECK(plan.instructions[0].op == OpCode::LoadField);
        CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(c.expected));
      }
    }
  }

  TEST_CASE("ExecutionPlan - compiles boolean false to constant zero", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("false");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    REQUIRE_FALSE(plan.instructions.empty());
    CHECK(plan.instructions[0].op == OpCode::LoadConstant);
    CHECK(plan.instructions[0].constValue == 0);
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - rejects invalid AST nodes", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};

    SECTION("Unsupported variable type")
    {
      auto var = VariableExpression{.type = static_cast<VariableType>(99), .name = "invalid"};
      std::ignore = compileError(compiler, var);
    }

    SECTION("Unsupported operator in BinaryExpression")
    {
      auto binaryPtr = std::make_unique<BinaryExpression>();
      binaryPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "title"};
      binaryPtr->optOperation =
        BinaryExpression::Operation{.op = Operator::Add, .operand = ConstantExpression{std::int64_t{100}}};

      std::ignore = compileError(compiler, std::move(binaryPtr));

      auto binaryInvalidPtr = std::make_unique<BinaryExpression>();
      binaryInvalidPtr->operand = VariableExpression{.type = VariableType::Metadata, .name = "title"};
      binaryInvalidPtr->optOperation =
        BinaryExpression::Operation{.op = static_cast<Operator>(99), .operand = ConstantExpression{std::int64_t{100}}};

      std::ignore = compileError(compiler, std::move(binaryInvalidPtr));
    }

    SECTION("Compiler rejects unsupported unary operators")
    {
      auto unaryPtr = std::make_unique<UnaryExpression>();
      unaryPtr->op = Operator::Add; // Unsupported unary operator
      unaryPtr->operand = VariableExpression{.type = VariableType::Tag, .name = "rock"};

      std::ignore = compileError(compiler, std::move(unaryPtr));
    }
  }

  TEST_CASE("ExecutionPlan - deduplicates string constants", "[query][unit][execution_plan]")
  {
    SECTION("Reuses Identical String Constants")
    {
      auto expr = parseOk(R"($title = "Bach" or $title != "Bach")");
      auto compiler = QueryCompiler{};
      auto plan = compileOk(compiler, expr);
      CHECK(plan.stringConstants.size() == 1);
      CHECK(plan.stringConstants[0] == "Bach");
    }

    SECTION("Stores Different String Constants Separately")
    {
      auto expr = parseOk(R"($title = "Bach" or $title = "Mozart")");
      auto compiler = QueryCompiler{};
      auto plan = compileOk(compiler, expr);
      CHECK(plan.stringConstants.size() == 2);
    }
  }
} // namespace ao::query::test
