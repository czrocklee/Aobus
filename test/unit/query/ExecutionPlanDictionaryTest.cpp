// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/query/ExecutionPlanTestSupport.h"
#include <ao/query/Field.h>
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ao::query::test
{
  namespace
  {
    Instruction const& findInstruction(ExecutionPlan const& plan, OpCode op)
    {
      for (auto const& instruction : plan.instructions)
      {
        if (instruction.op == op)
        {
          return instruction;
        }
      }

      FAIL("expected instruction was not compiled");
      return plan.instructions.front();
    }
  } // namespace

  TEST_CASE("ExecutionPlan - compiles custom field existence as an owned symbol", "[query][unit][execution-plan]")
  {
    auto const plan = compileOk(QueryCompiler{}, parseOk("%rating?"));

    REQUIRE(plan.instructions.size() == 1);
    REQUIRE(plan.dictionarySymbols == std::vector<std::string>{"rating"});
    CHECK(plan.instructions[0].op == OpCode::Exists);
    CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::Custom));
    CHECK(plan.instructions[0].dictionarySymbol == 0);
    CHECK(plan.requiresDictionary);
    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - dictionary-backed LIKE keeps plan-owned text", "[query][unit][execution-plan]")
  {
    auto const plan = compileOk(QueryCompiler{}, parseOk(R"($artist ~ "Bach")"));

    REQUIRE(plan.stringConstants == std::vector<std::string>{"Bach"});
    CHECK(plan.dictionarySymbols.empty());
    CHECK(findInstruction(plan, OpCode::Like).field == static_cast<std::uint8_t>(Field::ArtistId));
    CHECK(plan.requiresDictionary);
  }

  TEST_CASE("ExecutionPlan - unknown tags compile without dictionary mutation", "[query][unit][execution-plan]")
  {
    auto expression = parseOk("#FutureTag");
    auto const plan = compileOk(QueryCompiler{}, expression);

    REQUIRE(plan.dictionarySymbols == std::vector<std::string>{"FutureTag"});
    REQUIRE(plan.requiredTagSymbols == std::vector<std::uint32_t>{0});
    CHECK(findInstruction(plan, OpCode::Eq).dictionarySymbol == 0);
    CHECK(plan.requiresDictionary);
  }

  TEST_CASE("ExecutionPlan - unknown custom keys compile without dictionary mutation", "[query][unit][execution-plan]")
  {
    auto const plan = compileOk(QueryCompiler{}, parseOk("%FutureKey = 'Value'"));

    REQUIRE(plan.dictionarySymbols == std::vector<std::string>{"FutureKey"});
    auto const& load = findInstruction(plan, OpCode::LoadField);
    CHECK(load.field == static_cast<std::uint8_t>(Field::Custom));
    CHECK(load.dictionarySymbol == 0);
    CHECK(findInstruction(plan, OpCode::Eq).dictionarySymbol == 0);
  }

  TEST_CASE("ExecutionPlan - dictionary-backed equality compiles to a bindable symbol", "[query][unit][execution-plan]")
  {
    auto const plan = compileOk(QueryCompiler{}, parseOk("$artist = 'Bach'"));

    REQUIRE(plan.dictionarySymbols == std::vector<std::string>{"Bach"});
    CHECK(plan.stringConstants.empty());
    CHECK(findInstruction(plan, OpCode::Eq).dictionarySymbol == 0);
    CHECK(plan.requiresDictionary);
  }
} // namespace ao::query::test
