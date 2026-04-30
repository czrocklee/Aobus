// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators_all.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>

#include <rs/expr/ExecutionPlan.h>
#include <rs/expr/Parser.h>
#include <rs/library/DictionaryStore.h>
#include <rs/lmdb/Database.h>
#include <rs/lmdb/Environment.h>
#include <rs/lmdb/Transaction.h>

#include <test/unit/lmdb/TestUtils.h>

using namespace rs::expr;

TEST_CASE("ExecutionPlan - Compile Simple Expression")
{
  auto expr = parse("$artist = Bach");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
  CHECK_FALSE(plan.matchesAll);
}

TEST_CASE("ExecutionPlan - Compile Empty Expression")
{
  // Note: matchesAll is not automatically set - it's a hint for optimization
  // The plan should still compile a constant true expression
  auto expr = parse("true");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  // The plan should have at least one instruction (LoadConstant)
  CHECK(plan.instructions.size() >= 1);
}

TEST_CASE("ExecutionPlan - Compile Metadata Field")
{
  auto expr = parse("$title = 'Test'");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

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

TEST_CASE("ExecutionPlan - Compile Property Field")
{
  auto expr = parse("@duration > 180000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

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

TEST_CASE("ExecutionPlan - Property Alias Maps To Bitrate Field")
{
  auto expr = parse("@br >= 320k");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  REQUIRE_FALSE(plan.instructions.empty());
  CHECK(plan.instructions[0].op == OpCode::LoadField);
  CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::Bitrate));
}

TEST_CASE("ExecutionPlan - Metadata Alias Maps To AlbumArtist Field")
{
  auto expr = parse("$aa = Bach");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  REQUIRE_FALSE(plan.instructions.empty());
  CHECK(plan.instructions[0].op == OpCode::LoadField);
  CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::AlbumArtistId));
}

TEST_CASE("ExecutionPlan - Duration Unit Constant")
{
  auto expr = parse("@duration >= 3m");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  auto const it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

  REQUIRE(it != plan.instructions.end());
  CHECK(it->constValue == 180000);
}

TEST_CASE("ExecutionPlan - Bitrate Unit Constant")
{
  auto expr = parse("@bitrate >= 2m");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  auto const it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

  REQUIRE(it != plan.instructions.end());
  CHECK(it->constValue == 2000000);
}

TEST_CASE("ExecutionPlan - SampleRate Unit Constant")
{
  auto expr = parse("@sampleRate = 44.1k");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  auto const it = std::ranges::find(plan.instructions, OpCode::LoadConstant, &Instruction::op);

  REQUIRE(it != plan.instructions.end());
  CHECK(it->constValue == 44100);
}

TEST_CASE("ExecutionPlan - Unit Constant Rejects Unsupported Field")
{
  auto expr = parse("$year >= 3m");
  auto compiler = QueryCompiler{};

  REQUIRE_THROWS(compiler.compile(expr));
}

TEST_CASE("ExecutionPlan - Compile Logical And")
{
  // Use && for logical and to ensure it's parsed correctly
  auto expr = parse("$artist = Bach && $genre = Classical");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

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

TEST_CASE("ExecutionPlan - Compile Logical Or")
{
  // Use || for logical or to ensure it's parsed correctly
  auto expr = parse("$artist = Bach || $artist = Mozart");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

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

TEST_CASE("ExecutionPlan - Compile Logical Not")
{
  auto expr = parse("not $artist");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

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

TEST_CASE("ExecutionPlan - Compile Relational Operators")
{
  auto expr = parse("$year < 2000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

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

  expr = parse("$year <= 2000");
  compiler = QueryCompiler();
  plan = compiler.compile(expr);

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

TEST_CASE("ExecutionPlan - Compile Like")
{
  auto expr = parse("$title ~ Love");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

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

TEST_CASE("ExecutionPlan - Compile String Constant")
{
  auto expr = parse("$title = 'Hello World'");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.stringConstants.empty());
  CHECK(plan.stringConstants[0] == "Hello World");
}

TEST_CASE("ExecutionPlan - Field Enum Values")
{
  // String fields
  CHECK(static_cast<std::uint8_t>(Field::Title) == 0);
  CHECK(static_cast<std::uint8_t>(Field::Uri) == 1);

  // Property fields
  CHECK(static_cast<std::uint8_t>(Field::DurationMs) == 2);
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

TEST_CASE("ExecutionPlan - OpCode Enum Values")
{
  CHECK(static_cast<std::uint8_t>(OpCode::Nop) == 0);
  CHECK(static_cast<std::uint8_t>(OpCode::LoadField) == 1);
  CHECK(static_cast<std::uint8_t>(OpCode::LoadConstant) == 2);
  CHECK(static_cast<std::uint8_t>(OpCode::Eq) == 3);
  CHECK(static_cast<std::uint8_t>(OpCode::Ne) == 4);
}

TEST_CASE("ExecutionPlan - AccessProfile HotOnly")
{
  // Metadata variable -> HotOnly
  auto expr = parse("$artist = Bach");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.accessProfile == AccessProfile::HotOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile ColdOnly")
{
  // Custom variable -> ColdOnly
  auto expr = parse("%customkey = value");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.accessProfile == AccessProfile::ColdOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile HotAndCold")
{
  // Mix of hot and cold -> HotAndCold
  auto expr = parse("$artist = Bach && %customkey = value");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.accessProfile == AccessProfile::HotAndCold);
}

TEST_CASE("ExecutionPlan - AccessProfile Property Field")
{
  // Property variable -> ColdOnly (stored in TrackColdHeader)
  auto expr = parse("@duration > 180000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.accessProfile == AccessProfile::ColdOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile Tag Field")
{
  // Tag variable -> HotOnly
  auto expr = parse("#rock");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.accessProfile == AccessProfile::HotOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile Cold Field")
{
  // TrackNumber field is in cold storage -> ColdOnly
  auto expr = parse("$trackNumber > 5");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.accessProfile == AccessProfile::ColdOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile DurationMs is ColdOnly")
{
  auto expr = parse("@duration > 180000");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  CHECK(plan.accessProfile == AccessProfile::ColdOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile Bitrate is ColdOnly")
{
  auto expr = parse("@bitrate > 320");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  CHECK(plan.accessProfile == AccessProfile::ColdOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile SampleRate is ColdOnly")
{
  auto expr = parse("@sampleRate = 44100");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  CHECK(plan.accessProfile == AccessProfile::ColdOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile Channels is ColdOnly")
{
  auto expr = parse("@channels = 2");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);
  CHECK(plan.accessProfile == AccessProfile::ColdOnly);
}

TEST_CASE("ExecutionPlan - AccessProfile Mixed HotAndCold")
{
  // Mix of hot ($year) and cold ($trackNumber) -> HotAndCold
  auto expr = parse("$year > 2020 && $trackNumber > 5");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.accessProfile == AccessProfile::HotAndCold);
}

TEST_CASE("ExecutionPlan - AccessProfile Custom Field")
{
  // Custom variable -> ColdOnly
  auto expr = parse("%customkey = value");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK(plan.accessProfile == AccessProfile::ColdOnly);
}

TEST_CASE("ExecutionPlan - LIKE operator works for ArtistId")
{
  auto temp = TempDir{};
  auto env = rs::lmdb::Environment{temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20}};
  auto wtxn = rs::lmdb::WriteTransaction{env};
  auto dict = rs::library::DictionaryStore{rs::lmdb::Database{wtxn, "dict"}, wtxn};
  dict.put(wtxn, "Johann Sebastian Bach");

  auto expr = parse(R"($artist ~ "Bach")");
  auto compiler = QueryCompiler{&dict};
  auto plan = compiler.compile(expr);

  REQUIRE(plan.dictionary == &dict);
  REQUIRE(plan.stringConstants.size() == 1);
  CHECK(plan.stringConstants.front() == "Bach");
  CHECK(plan.instructions.back().op == OpCode::Like);
  CHECK(plan.instructions.back().field == static_cast<std::uint8_t>(Field::ArtistId));
}

TEST_CASE("ExecutionPlan - LIKE operator works for AlbumId")
{
  auto expr = parse(R"($album ~ "Greatest Hits")");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
}

TEST_CASE("ExecutionPlan - LIKE operator works for GenreId")
{
  auto expr = parse(R"($genre ~ "Rock")");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
}

TEST_CASE("ExecutionPlan - LIKE operator works for AlbumArtistId")
{
  auto expr = parse(R"($albumArtist ~ "Bach")");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
}

TEST_CASE("ExecutionPlan - LIKE operator not supported for CoverArtId")
{
  auto expr = parse(R"($coverArt ~ "front")");
  auto compiler = QueryCompiler{};
  REQUIRE_THROWS(compiler.compile(expr));
}

TEST_CASE("ExecutionPlan - LIKE operator not supported for Tags")
{
  auto expr = parse(R"(#rock ~ "progressive")");
  auto compiler = QueryCompiler{};
  REQUIRE_THROWS(compiler.compile(expr));
}

TEST_CASE("ExecutionPlan - LIKE operator works for Title")
{
  auto expr = parse(R"($title ~ "Bach")");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
  CHECK_FALSE(plan.matchesAll);
}

TEST_CASE("ExecutionPlan - LIKE operator works for Uri")
{
  auto expr = parse(R"($uri ~ "/music/")");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
  CHECK_FALSE(plan.matchesAll);
}

TEST_CASE("ExecutionPlan - Mixed LIKE and EQUAL in OR expression")
{
  // This tests that leftField is correctly saved before compiling right operand
  // $title ~ "Bach" should NOT check if ArtistId is used with LIKE
  auto expr = parse(R"($title ~ "Bach" or $artist = "Bach")");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
  CHECK_FALSE(plan.matchesAll);
}

TEST_CASE("ExecutionPlan - Parenthesized LIKE and EQUAL in OR expression")
{
  // Explicit grouping with parentheses should also work
  auto expr = parse(R"(($title ~ "Bach") or ($artist = "Bach"))");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
  CHECK_FALSE(plan.matchesAll);
}

TEST_CASE("ExecutionPlan - Multiple OR with ID field equality")
{
  // Multiple ID field equalities in OR should compile without throwing
  auto expr = parse(R"($artist = "Bach" or $artist = "Mozart" or $album = "交响乐")");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
  CHECK_FALSE(plan.matchesAll);
}

TEST_CASE("ExecutionPlan - Title LIKE chained with AND")
{
  // Title LIKE should work with AND
  auto expr = parse(R"($title ~ "Bach" and $year > 2000)");
  auto compiler = QueryCompiler{};
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
  CHECK_FALSE(plan.matchesAll);
}
