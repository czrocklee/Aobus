// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/AudioCodec.h>
#include <ao/Error.h>
#include <ao/library/DictionaryStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/query/Expression.h>
#include <ao/query/Field.h>
#include <ao/query/Parser.h>
#include <ao/query/QueryCompiler.h>
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <lmdb.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <tuple>
#include <utility>

namespace ao::query::test
{
  using namespace ao::lmdb::test;

  namespace
  {
    Expression parseOk(std::string_view text)
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
  } // namespace

  TEST_CASE("ExecutionPlan - Compile Simple Expression", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("$artist = Bach");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - Compile Constant True Expression", "[query][unit][execution_plan]")
  {
    // Note: matchesAll is not automatically set - it's a hint for optimization
    // The plan should still compile a constant true expression
    auto expr = parseOk("true");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    // The plan should have at least one instruction (LoadConstant)
    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - Compile Metadata Field", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Compile Property Field", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Property Alias Maps To Bitrate Field", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("@br >= 320k");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    REQUIRE_FALSE(plan.instructions.empty());
    CHECK(plan.instructions[0].op == OpCode::LoadField);
    CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::Bitrate));
  }

  TEST_CASE("ExecutionPlan - Metadata Alias Maps To AlbumArtist Field", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("$aa = Bach");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    REQUIRE_FALSE(plan.instructions.empty());
    CHECK(plan.instructions[0].op == OpCode::LoadField);
    CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::AlbumArtistId));
  }

  TEST_CASE("ExecutionPlan - Duration Unit Constant", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("@duration >= 3m");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

    REQUIRE(it != plan.instructions.end());
    CHECK(it->constValue == 180000);
  }

  TEST_CASE("ExecutionPlan - Bitrate Unit Constant", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("@bitrate >= 2m");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

    REQUIRE(it != plan.instructions.end());
    CHECK(it->constValue == 2000000);
  }

  TEST_CASE("ExecutionPlan - SampleRate Unit Constant", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("@sampleRate = 44.1k");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

    REQUIRE(it != plan.instructions.end());
    CHECK(it->constValue == 44100);
  }

  TEST_CASE("ExecutionPlan - Codec Constant", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, parseOk("@codec = AAC"));

    auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

    REQUIRE(it != plan.instructions.end());
    CHECK(std::cmp_equal(it->constValue, audioCodecStorageValue(AudioCodec::Aac)));
  }

  TEST_CASE("ExecutionPlan - Unsupported Codec Constant", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};

    std::ignore = compileError(compiler, parseOk("@codec = OPUS"));
  }

  TEST_CASE("ExecutionPlan - Unit Constant Rejects Unsupported Field", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("$year >= 3m");
    auto compiler = QueryCompiler{};

    std::ignore = compileError(compiler, expr);
  }

  TEST_CASE("ExecutionPlan - Compile Logical And", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Compile Logical Or", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Compile In List", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Compile In Range", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Ordered Comparison Field Restrictions", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Compile Logical Not", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Compile Existence Tests", "[query][unit][execution_plan]")
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

    SECTION("CustomExistenceCarriesDictionaryId")
    {
      auto temp = ao::test::TempDir{};
      auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
      auto wtxn = lmdb::test::beginWriteTransaction(env);
      auto dict = library::DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
      auto dictCompiler = QueryCompiler{&dict};

      auto const plan = compileOk(dictCompiler, parseOk("%rating?"));

      REQUIRE(plan.instructions.size() == 1);
      CHECK(plan.instructions[0].op == OpCode::Exists);
      CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::Custom));
      CHECK(std::cmp_equal(plan.instructions[0].constValue, dict.getId("rating").raw()));
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

  TEST_CASE("ExecutionPlan - Compile Relational Operators", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Compile Like", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Compile String Constant", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("$title = 'Hello World'");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.stringConstants.empty());
    CHECK(plan.stringConstants[0] == "Hello World");
  }

  TEST_CASE("ExecutionPlan - Field Enum Values", "[query][unit][execution_plan]")
  {
    // String fields
    CHECK(static_cast<std::uint8_t>(Field::Title) == 0);
    CHECK(static_cast<std::uint8_t>(Field::Uri) == 1);

    // Property fields
    CHECK(static_cast<std::uint8_t>(Field::Duration) == 2);
    CHECK(static_cast<std::uint8_t>(Field::Bitrate) == 3);
    CHECK(static_cast<std::uint8_t>(Field::SampleRate) == 4);

    // Metadata ID fields
    CHECK(static_cast<std::uint8_t>(Field::ArtistId) == 9);
    CHECK(static_cast<std::uint8_t>(Field::AlbumId) == 10);
    CHECK(static_cast<std::uint8_t>(Field::ComposerId) == 13);
    CHECK(static_cast<std::uint8_t>(Field::WorkId) == 15);

    // Metadata numeric fields
    CHECK(static_cast<std::uint8_t>(Field::Year) == 16);
    CHECK(static_cast<std::uint8_t>(Field::TrackNumber) == 17);

    // Tag fields
    CHECK(static_cast<std::uint8_t>(Field::TagBloom) == 21);
    CHECK(static_cast<std::uint8_t>(Field::TagCount) == 22);
  }

  TEST_CASE("ExecutionPlan - OpCode Enum Values", "[query][unit][execution_plan]")
  {
    CHECK(static_cast<std::uint8_t>(OpCode::Nop) == 0);
    CHECK(static_cast<std::uint8_t>(OpCode::LoadField) == 1);
    CHECK(static_cast<std::uint8_t>(OpCode::LoadConstant) == 2);
    CHECK(static_cast<std::uint8_t>(OpCode::Eq) == 3);
    CHECK(static_cast<std::uint8_t>(OpCode::Ne) == 4);
  }

  TEST_CASE("ExecutionPlan - AccessProfile HotOnly", "[query][unit][execution_plan]")
  {
    // Metadata variable -> HotOnly
    auto expr = parseOk("$artist = Bach");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.accessProfile == AccessProfile::HotOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile ColdOnly", "[query][unit][execution_plan]")
  {
    // Custom variable -> ColdOnly
    auto expr = parseOk("%customkey = value");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile HotAndCold", "[query][unit][execution_plan]")
  {
    // Mix of hot and cold -> HotAndCold
    auto expr = parseOk("$artist = Bach && %customkey = value");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.accessProfile == AccessProfile::HotAndCold);
  }

  TEST_CASE("ExecutionPlan - AccessProfile Property Field", "[query][unit][execution_plan]")
  {
    // Property variable -> ColdOnly (stored in TrackColdHeader)
    auto expr = parseOk("@duration > 180000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile Tag Field", "[query][unit][execution_plan]")
  {
    // Tag variable -> HotOnly
    auto expr = parseOk("#rock");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.accessProfile == AccessProfile::HotOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile Cold Field", "[query][unit][execution_plan]")
  {
    // TrackNumber field is in cold storage -> ColdOnly
    auto expr = parseOk("$trackNumber > 5");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile Duration is ColdOnly", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("@duration > 180000");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile Bitrate is ColdOnly", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("@bitrate > 320");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile SampleRate is HotOnly", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("@sampleRate = 44100");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    CHECK(plan.accessProfile == AccessProfile::HotOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile Channels is ColdOnly", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("@channels = 2");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - AccessProfile Mixed HotAndCold", "[query][unit][execution_plan]")
  {
    // Mix of hot ($year) and cold ($trackNumber) -> HotAndCold
    auto expr = parseOk("$year > 2020 && $trackNumber > 5");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.accessProfile == AccessProfile::HotAndCold);
  }

  TEST_CASE("ExecutionPlan - AccessProfile Custom Field", "[query][unit][execution_plan]")
  {
    // Custom variable -> ColdOnly
    auto expr = parseOk("%customkey = value");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - LIKE operator works for ArtistId", "[query][unit][execution_plan]")
  {
    auto temp = ao::test::TempDir{};
    auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = lmdb::test::beginWriteTransaction(env);
    auto dict = library::DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
    REQUIRE(dict.put(wtxn, "Johann Sebastian Bach"));

    auto expr = parseOk(R"($artist ~ "Bach")");
    auto compiler = QueryCompiler{&dict};

    if (auto const plan = compileOk(compiler, expr); plan.dictionary != nullptr)
    {
      REQUIRE(plan.dictionary == &dict);
      REQUIRE(plan.stringConstants.size() == 1);
      CHECK(plan.stringConstants.front() == "Bach");
      CHECK(plan.instructions.back().op == OpCode::Like);
      CHECK(plan.instructions.back().field == static_cast<std::uint8_t>(Field::ArtistId));
    }
  }

  TEST_CASE("ExecutionPlan - LIKE operator works for AlbumId", "[query][unit][execution_plan]")
  {
    auto expr = parseOk(R"($album ~ "Greatest Hits")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - LIKE operator works for GenreId", "[query][unit][execution_plan]")
  {
    auto expr = parseOk(R"($genre ~ "Rock")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - LIKE operator works for AlbumArtistId", "[query][unit][execution_plan]")
  {
    auto expr = parseOk(R"($albumArtist ~ "Bach")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
  }

  TEST_CASE("ExecutionPlan - LIKE operator not supported for CoverArtId", "[query][unit][execution_plan]")
  {
    auto expr = parseOk(R"($coverArt ~ "front")");
    auto compiler = QueryCompiler{};
    std::ignore = compileError(compiler, expr);
  }

  TEST_CASE("ExecutionPlan - LIKE operator not supported for Tags", "[query][unit][execution_plan]")
  {
    auto expr = parseOk(R"(#rock ~ "progressive")");
    auto compiler = QueryCompiler{};
    std::ignore = compileError(compiler, expr);
  }

  TEST_CASE("ExecutionPlan - LIKE operator works for Title", "[query][unit][execution_plan]")
  {
    auto expr = parseOk(R"($title ~ "Bach")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - Unknown Metadata Field Throws", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("$uri = 'x'");
    auto compiler = QueryCompiler{};
    std::ignore = compileError(compiler, expr);
  }

  TEST_CASE("ExecutionPlan - Unknown Property Field Throws", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("@tagCount > 0");
    auto compiler = QueryCompiler{};
    std::ignore = compileError(compiler, expr);
  }

  TEST_CASE("ExecutionPlan - Add Operator Is Rejected", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("$title + $artist");
    auto compiler = QueryCompiler{};
    std::ignore = compileError(compiler, expr);
  }

  TEST_CASE("ExecutionPlan - Mixed LIKE and EQUAL in OR expression", "[query][unit][execution_plan]")
  {
    // This tests that leftField is correctly saved before compiling right operand
    // $title ~ "Bach" should NOT check if ArtistId is used with LIKE
    auto expr = parseOk(R"($title ~ "Bach" or $artist = "Bach")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - Parenthesized LIKE and EQUAL in OR expression", "[query][unit][execution_plan]")
  {
    // Explicit grouping with parentheses should also work
    auto expr = parseOk(R"(($title ~ "Bach") or ($artist = "Bach"))");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - Multiple OR with ID field equality", "[query][unit][execution_plan]")
  {
    // Multiple ID field equalities in OR should compile without throwing
    auto expr = parseOk(R"($artist = "Bach" or $artist = "Mozart" or $album = "交响乐")");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - Title LIKE chained with AND", "[query][unit][execution_plan]")
  {
    // Title LIKE should work with AND
    auto expr = parseOk(R"($title ~ "Bach" and $year > 2000)");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);

    CHECK_FALSE(plan.instructions.empty());
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - Future matching for tags not yet in dictionary", "[query][unit][execution_plan]")
  {
    auto temp = ao::test::TempDir{};
    auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = lmdb::test::beginWriteTransaction(env);
    auto dict = library::DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};

    // Tag "FutureTag" does not exist in dictionary yet
    auto expr = parseOk("#FutureTag");
    auto compiler = QueryCompiler{&dict};

    // Compile the plan. This should use getOrIntern() to allocate a stable ID
    auto plan = compileOk(compiler, expr);

    // The ID should now be in the dictionary because of getOrIntern()
    CHECK(dict.contains("FutureTag"));
    auto futureTagId = dict.getId("FutureTag");

    // Verify that the instruction uses this ID
    bool foundTagEq = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::Eq)
      {
        // The register before Eq should contain the constant we loaded
        auto const& loadInstr = plan.instructions[static_cast<std::size_t>(&instr - plan.instructions.data()) - 1U];

        if (loadInstr.op == OpCode::LoadConstant)
        {
          CHECK(std::cmp_equal(loadInstr.constValue, futureTagId.raw()));
          foundTagEq = true;
        }
      }
    }

    CHECK(foundTagEq);

    // Verify Bloom Filter also contains the bit for this getOrInternd ID
    std::uint32_t const expectedBit = std::uint32_t{1} << (futureTagId.raw() & 31); // 31 is kBloomBitMask
    CHECK((plan.tagBloomMask & expectedBit) == expectedBit);
  }

  TEST_CASE("ExecutionPlan - Future matching for custom fields not yet in dictionary", "[query][unit][execution_plan]")
  {
    auto temp = ao::test::TempDir{};
    auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = lmdb::test::beginWriteTransaction(env);
    auto dict = library::DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};

    // Custom field "FutureKey" does not exist
    auto expr = parseOk("%FutureKey = 'Value'");
    auto compiler = QueryCompiler{&dict};

    auto plan = compileOk(compiler, expr);

    // ID should have been getOrInternd
    CHECK(dict.contains("FutureKey"));
    auto futureKeyId = dict.getId("FutureKey");

    // Verify that the instruction uses this ID
    bool foundLoadField = false;

    for (auto const& instr : plan.instructions)
    {
      if (instr.op == OpCode::LoadField && instr.field == static_cast<std::uint8_t>(Field::Custom))
      {
        CHECK(std::cmp_equal(instr.constValue, futureKeyId.raw()));
        foundLoadField = true;
      }
    }

    CHECK(foundLoadField);
  }

  TEST_CASE("ExecutionPlan - Metadata Catalog Maps Every Supported Name", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Property Catalog Maps Every Supported Name", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - AccessProfile Exhaustive Classification", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};

    SECTION("HotOnly")
    {
      auto fields = {"$title",
                     "$artist",
                     "$album",
                     "$genre",
                     "$albumArtist",
                     "$composer",
                     "$year",
                     "#rock",
                     "true",
                     "false",
                     "@sampleRate",
                     "@bitDepth",
                     "@codec"};

      for (auto const* f : fields)
      {
        auto expr = parseOk(f);

        if (f[0] != '#' && std::string{f} != "true" && std::string{f} != "false")
        {
          expr = parseOk(std::string{f} + " = 0");
        }

        auto plan = compileOk(compiler, expr);
        CHECK(plan.accessProfile == AccessProfile::HotOnly);
      }
    }

    SECTION("ColdOnly")
    {
      auto fields = {"$trackNumber",
                     "$trackTotal",
                     "$discNumber",
                     "$discTotal",
                     "$coverArt",
                     "%isrc",
                     "@duration",
                     "@bitrate",
                     "@channels"};

      for (auto const* f : fields)
      {
        auto expr = parseOk(std::string{f} + " >= 0");
        auto plan = compileOk(compiler, expr);
        CHECK(plan.accessProfile == AccessProfile::ColdOnly);
      }

      // $work is a dictionary field (cold), so reference it with equality rather
      // than an ordered comparison, which is rejected for dictionary fields.
      auto workPlan = compileOk(compiler, parseOk("$work = w"));
      CHECK(workPlan.accessProfile == AccessProfile::ColdOnly);
    }

    SECTION("HotAndCold")
    {
      auto expr = parseOk("$year >= 2020 and @duration >= 3m");
      auto plan = compileOk(compiler, expr);
      CHECK(plan.accessProfile == AccessProfile::HotAndCold);
    }
  }

  TEST_CASE("ExecutionPlan - Boolean False Compiles To ConstantZero", "[query][unit][execution_plan]")
  {
    auto expr = parseOk("false");
    auto compiler = QueryCompiler{};
    auto plan = compileOk(compiler, expr);
    REQUIRE_FALSE(plan.instructions.empty());
    CHECK(plan.instructions[0].op == OpCode::LoadConstant);
    CHECK(plan.instructions[0].constValue == 0);
    CHECK_FALSE(plan.matchesAll);
  }

  TEST_CASE("ExecutionPlan - Invalid AST Nodes Throw", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - String Constant Deduplication", "[query][unit][execution_plan]")
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

  TEST_CASE("ExecutionPlan - Unit Literal Scaling", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};

    SECTION("Duration Supports MsSMMHUnits")
    {
      struct Case final
      {
        std::string unit;
        std::int64_t expected;
      };
      auto cases = {Case{.unit = "1ms", .expected = 1},
                    Case{.unit = "1s", .expected = 1000},
                    Case{.unit = "1m", .expected = 60000},
                    Case{.unit = "1h", .expected = 3600000}};

      for (auto const& c : cases)
      {
        auto expr = parseOk("@duration >= " + c.unit);
        auto plan = compileOk(compiler, expr);
        auto it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);
        CHECK(it->constValue == c.expected);
      }
    }

    SECTION("DurationSupportsCompoundUnits")
    {
      auto expr = parseOk("@duration >= 2m30s");
      CHECK(compileOk(compiler, expr).instructions[1].constValue == 150000);
    }

    SECTION("Bitrate and SampleRate Support KAndMUnits")
    {
      auto expr1 = parseOk("@bitrate >= 256k");
      CHECK(compileOk(compiler, expr1).instructions[1].constValue == 256000);

      auto expr2 = parseOk("@sampleRate >= 44.1k");
      CHECK(compileOk(compiler, expr2).instructions[1].constValue == 44100);
    }

    SECTION("Unit Suffix Is CaseInsensitive")
    {
      auto expr = parseOk("@bitrate >= 256K");
      CHECK(compileOk(compiler, expr).instructions[1].constValue == 256000);
    }

    SECTION("Negative Unit Literal Compiles")
    {
      auto expr = parseOk("@bitrate >= -2k");
      CHECK(compileOk(compiler, expr).instructions[1].constValue == -2000);
    }
  }

  TEST_CASE("ExecutionPlan - Unit Literal Error Paths", "[query][unit][execution_plan]")
  {
    auto compiler = QueryCompiler{};

    SECTION("Rejects UnsupportedSuffixForField")
    {
      std::ignore = compileError(compiler, parseOk("@duration >= 10k"));
      std::ignore = compileError(compiler, parseOk("@bitrate >= 3h"));
      std::ignore = compileError(compiler, parseOk("@sampleRate >= 44h"));
      std::ignore = compileError(compiler, parseOk("@channels = 2h"));
      std::ignore = compileError(compiler, parseOk("@bitDepth = 16h"));
      std::ignore = compileError(compiler, parseOk("$year = 2020h"));
      std::ignore = compileError(compiler, parseOk("$trackNumber = 1h"));
      std::ignore = compileError(compiler, parseOk("$trackTotal = 10h"));
      std::ignore = compileError(compiler, parseOk("$discNumber = 1h"));
      std::ignore = compileError(compiler, parseOk("$discTotal = 2h"));
      std::ignore = compileError(compiler, parseOk("%custom = 1h"));
    }

    SECTION("Rejects OutOfRangeIntegerParsing")
    {
      std::ignore = compileError(compiler, parseOk("@bitrate >= 9999999999999999999999k"));

      // checkedMul overflow (value * 1000 overflows)
      std::ignore = compileError(compiler, parseOk("@duration >= 1844674407370955161s"));

      // checkedAdd overflow (value * 10 + fraction overflows)
      std::ignore = compileError(compiler, parseOk("@duration >= 1844674407370955161.6ms"));
    }

    SECTION("Rejects NonIntegerResolution")
    {
      std::ignore = compileError(compiler, parseOk("@duration >= 1.5ms"));
    }

    SECTION("RejectsCompoundUnitsOutsideDuration")
    {
      std::ignore = compileError(compiler, parseOk("@bitrate >= 2k3m"));
    }

    SECTION("Accepts Zero")
    {
      auto plan = compileOk(compiler, parseOk("@duration >= 0s"));
      CHECK(plan.instructions[1].constValue == 0);
    }

    SECTION("Rejects MissingNumericFieldContext")
    {
      // A top-level unit constant expression like "3m" should fail
      std::ignore = compileError(compiler, parseOk("3m"));
    }
  }

  TEST_CASE("ExecutionPlan - Tag Bloom Mask Compilation", "[query][unit][execution_plan]")
  {
    auto temp = ao::test::TempDir{};
    auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = lmdb::test::beginWriteTransaction(env);
    auto dict = library::DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};

    auto rockId = ao::test::requireValue(dict.put(wtxn, "rock"));
    auto jazzId = ao::test::requireValue(dict.put(wtxn, "jazz"));
    wtxn.commit();

    std::uint32_t const rockBit = std::uint32_t{1} << (rockId.raw() & 31);
    std::uint32_t const jazzBit = std::uint32_t{1} << (jazzId.raw() & 31);

    SECTION("Tag Bloom Mask For SingleTagWithDictionary")
    {
      auto plan = compileOk(QueryCompiler{&dict}, parseOk("#rock"));
      CHECK(plan.tagBloomMask == rockBit);
    }

    SECTION("Tag Bloom Mask Ors Tags Across And")
    {
      auto plan = compileOk(QueryCompiler{&dict}, parseOk("#rock and #jazz"));
      CHECK(plan.tagBloomMask == (rockBit | jazzBit));
    }

    SECTION("Tag Bloom Mask Intersects Tags Across Or")
    {
      auto plan = compileOk(QueryCompiler{&dict}, parseOk("#rock or #jazz"));
      CHECK(plan.tagBloomMask == (rockBit & jazzBit));
    }

    SECTION("Tag Bloom Mask Clears Under Not")
    {
      auto plan = compileOk(QueryCompiler{&dict}, parseOk("not #rock"));
      CHECK(plan.tagBloomMask == 0);
    }
  }

  TEST_CASE("ExecutionPlan - Dictionary-Backed Field Resolution", "[query][unit][execution_plan]")
  {
    auto temp = ao::test::TempDir{};
    auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = lmdb::test::beginWriteTransaction(env);
    auto dict = library::DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};
    auto bachId = ao::test::requireValue(dict.put(wtxn, "Bach"));
    wtxn.commit();

    SECTION("Dictionary-Backed Equality Resolves To NumericId")
    {
      auto expr = parseOk("$artist = \"Bach\"");
      auto compiler = QueryCompiler{&dict};
      auto plan = compileOk(compiler, expr);
      REQUIRE(plan.instructions.size() >= 2);
      CHECK(plan.instructions[1].op == OpCode::LoadConstant);
      CHECK(std::cmp_equal(plan.instructions[1].constValue, bachId.raw()));
      CHECK(plan.stringConstants.empty());
    }

    SECTION("Dictionary-Backed Like Keeps StringConstant")
    {
      auto expr = parseOk("$artist ~ \"Bach\"");
      auto compiler = QueryCompiler{&dict};
      auto plan = compileOk(compiler, expr);
      REQUIRE(plan.instructions.size() >= 2);
      CHECK(plan.instructions[1].op == OpCode::LoadConstant);
      // When using LIKE, we don't resolve to ID, so it should be a string constant index
      CHECK(plan.stringConstants.size() == 1);
      CHECK(plan.stringConstants[0] == "Bach");
    }

    SECTION("No Dictionary Leaves Metadata Equality As StringConstant")
    {
      auto expr = parseOk("$artist = \"Bach\"");
      auto compiler = QueryCompiler{}; // No dictionary
      auto plan = compileOk(compiler, expr);
      CHECK(plan.stringConstants.size() == 1);
      CHECK(plan.stringConstants[0] == "Bach");
    }
  }

  TEST_CASE("compileQuery - Returns Result Without Throwing", "[query][unit][execution_plan]")
  {
    SECTION("Valid predicate yields a plan")
    {
      auto const plan = compileQuery(parseOk("$year = 1990"));
      REQUIRE(plan.has_value());
      CHECK(plan->accessProfile != AccessProfile::NoTrackData);
    }

    SECTION("Non-predicate expression yields an Error")
    {
      auto const plan = compileQuery(parseOk("$year"));
      REQUIRE_FALSE(plan.has_value());
      CHECK(plan.error().code == Error::Code::FormatRejected);
      CHECK_FALSE(plan.error().message.empty());
    }
  }
} // namespace ao::query::test
