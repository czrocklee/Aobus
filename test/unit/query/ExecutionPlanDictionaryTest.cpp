// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/lmdb/LmdbTestSupport.h"
#include "test/unit/query/ExecutionPlanTestSupport.h"
#include <ao/library/DictionaryStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>
#include <ao/query/Field.h>
#include <ao/query/detail/Bytecode.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace ao::query::test
{
  using namespace ao::lmdb::test;

  TEST_CASE("ExecutionPlan - compiles custom field existence with dictionary IDs", "[query][unit][execution-plan]")
  {
    auto temp = ao::test::TempDir{};
    auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = lmdb::test::beginWriteTransaction(env);
    auto dictionary = library::DictionaryStore{lmdb::test::openDatabase(wtxn, "dictionary"), wtxn};
    auto dictionaryCompiler = QueryCompiler{&dictionary};

    auto const plan = compileOk(dictionaryCompiler, parseOk("%rating?"));

    REQUIRE(plan.instructions.size() == 1);
    CHECK(plan.instructions[0].op == OpCode::Exists);
    CHECK(plan.instructions[0].field == static_cast<std::uint8_t>(Field::Custom));
    CHECK(std::cmp_equal(plan.instructions[0].constValue, dictionary.lookupId("rating").raw()));
    CHECK(plan.accessProfile == AccessProfile::ColdOnly);
  }

  TEST_CASE("ExecutionPlan - compiles LIKE for artist ids", "[query][unit][execution-plan]")
  {
    auto temp = ao::test::TempDir{};
    auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = lmdb::test::beginWriteTransaction(env);
    auto dictionary = library::DictionaryStore{lmdb::test::openDatabase(wtxn, "dictionary"), wtxn};
    CHECK(dictionary.put(wtxn, "Johann Sebastian Bach"));

    auto expr = parseOk(R"($artist ~ "Bach")");
    auto compiler = QueryCompiler{&dictionary};

    if (auto const plan = compileOk(compiler, expr); plan.dictionary != nullptr)
    {
      CHECK(plan.dictionary == &dictionary);
      REQUIRE(plan.stringConstants.size() == 1);
      CHECK(plan.stringConstants.front() == "Bach");
      CHECK(plan.instructions.back().op == OpCode::Like);
      CHECK(plan.instructions.back().field == static_cast<std::uint8_t>(Field::ArtistId));
    }
  }

  TEST_CASE("ExecutionPlan - interns tags missing from the dictionary", "[query][unit][execution-plan]")
  {
    auto temp = ao::test::TempDir{};
    auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = lmdb::test::beginWriteTransaction(env);
    auto dictionary = library::DictionaryStore{lmdb::test::openDatabase(wtxn, "dictionary"), wtxn};

    // Tag "FutureTag" does not exist in dictionary yet
    auto expr = parseOk("#FutureTag");
    auto compiler = QueryCompiler{&dictionary};

    // Compile the plan. This should use getOrIntern() to allocate a stable ID
    auto plan = compileOk(compiler, expr);

    // The ID should now be in the dictionary because of getOrIntern()
    CHECK(dictionary.contains("FutureTag"));
    auto futureTagId = dictionary.lookupId("FutureTag");

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

  TEST_CASE("ExecutionPlan - interns custom fields missing from the dictionary", "[query][unit][execution-plan]")
  {
    auto temp = ao::test::TempDir{};
    auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = lmdb::test::beginWriteTransaction(env);
    auto dictionary = library::DictionaryStore{lmdb::test::openDatabase(wtxn, "dictionary"), wtxn};

    // Custom field "FutureKey" does not exist
    auto expr = parseOk("%FutureKey = 'Value'");
    auto compiler = QueryCompiler{&dictionary};

    auto plan = compileOk(compiler, expr);

    // ID should have been getOrInternd
    CHECK(dictionary.contains("FutureKey"));
    auto futureKeyId = dictionary.lookupId("FutureKey");

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

  TEST_CASE("ExecutionPlan - resolves dictionary-backed fields", "[query][unit][execution-plan]")
  {
    auto temp = ao::test::TempDir{};
    auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = lmdb::test::beginWriteTransaction(env);
    auto dictionary = library::DictionaryStore{lmdb::test::openDatabase(wtxn, "dictionary"), wtxn};
    auto bachId = ao::test::requireValue(dictionary.put(wtxn, "Bach"));
    REQUIRE(wtxn.commit());

    SECTION("Dictionary-Backed Equality Resolves To NumericId")
    {
      auto expr = parseOk("$artist = \"Bach\"");
      auto compiler = QueryCompiler{&dictionary};
      auto plan = compileOk(compiler, expr);
      REQUIRE(plan.instructions.size() >= 2);
      CHECK(plan.instructions[1].op == OpCode::LoadConstant);
      CHECK(std::cmp_equal(plan.instructions[1].constValue, bachId.raw()));
      CHECK(plan.stringConstants.empty());
    }

    SECTION("Dictionary-Backed Like Keeps StringConstant")
    {
      auto expr = parseOk("$artist ~ \"Bach\"");
      auto compiler = QueryCompiler{&dictionary};
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
} // namespace ao::query::test
