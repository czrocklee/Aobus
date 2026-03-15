/*
 * Copyright (C) 2025 RockStudio
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <catch2/catch.hpp>

#include <rs/expr/ExecutionPlan.h>
#include <rs/expr/Parser.h>

using namespace rs::expr;

TEST_CASE("ExecutionPlan - Compile Simple Expression")
{
  auto expr = parse("$artist = Bach");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.instructions.empty());
  CHECK_FALSE(plan.matchesAll);
}

TEST_CASE("ExecutionPlan - Compile Empty Expression")
{
  // Note: matchesAll is not automatically set - it's a hint for optimization
  // The plan should still compile a constant true expression
  auto expr = parse("true");
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);

  // The plan should have at least one instruction (LoadConstant)
  CHECK(plan.instructions.size() >= 1);
}

TEST_CASE("ExecutionPlan - Compile Metadata Field")
{
  auto expr = parse("$title = 'Test'");
  QueryCompiler compiler;
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
  QueryCompiler compiler;
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
  QueryCompiler compiler;
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
  QueryCompiler compiler;
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
  QueryCompiler compiler;
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
  QueryCompiler compiler;
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
  QueryCompiler compiler;
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
  QueryCompiler compiler;
  auto plan = compiler.compile(expr);

  CHECK_FALSE(plan.stringConstants.empty());
  CHECK(plan.stringConstants[0] == "Hello World");
}

TEST_CASE("ExecutionPlan - Field Enum Values")
{
  CHECK(static_cast<std::uint8_t>(Field::TagBloom) == 0);
  CHECK(static_cast<std::uint8_t>(Field::DurationMs) == 1);
  CHECK(static_cast<std::uint8_t>(Field::Bitrate) == 2);
  CHECK(static_cast<std::uint8_t>(Field::SampleRate) == 3);
  CHECK(static_cast<std::uint8_t>(Field::ArtistId) == 4);
}

TEST_CASE("ExecutionPlan - OpCode Enum Values")
{
  CHECK(static_cast<std::uint8_t>(OpCode::Nop) == 0);
  CHECK(static_cast<std::uint8_t>(OpCode::LoadField) == 1);
  CHECK(static_cast<std::uint8_t>(OpCode::LoadConstant) == 2);
  CHECK(static_cast<std::uint8_t>(OpCode::Eq) == 3);
  CHECK(static_cast<std::uint8_t>(OpCode::Ne) == 4);
}
