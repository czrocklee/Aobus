// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <catch2/catch.hpp>

#include <rs/expr/ExecutionPlan.h>
#include <rs/expr/Parser.h>

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

  // Metadata numeric fields
  CHECK(static_cast<std::uint8_t>(Field::Year) == 14);
  CHECK(static_cast<std::uint8_t>(Field::TrackNumber) == 15);

  // Tag fields
  CHECK(static_cast<std::uint8_t>(Field::TagBloom) == 19);
  CHECK(static_cast<std::uint8_t>(Field::TagCount) == 20);
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
