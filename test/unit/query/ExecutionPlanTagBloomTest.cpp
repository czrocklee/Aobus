// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/lmdb/TestUtils.h"
#include "test/unit/query/ExecutionPlanTestUtils.h"
#include <ao/library/DictionaryStore.h>
#include <ao/lmdb/Database.h>
#include <ao/lmdb/Environment.h>
#include <ao/lmdb/Transaction.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cstdint>

namespace ao::query::test
{
  using namespace ao::lmdb::test;

  TEST_CASE("ExecutionPlan - compiles tag bloom masks", "[query][unit][execution-plan]")
  {
    auto temp = ao::test::TempDir{};
    auto env = lmdb::test::openEnvironment(temp.path(), {.flags = MDB_CREATE, .maxDatabases = 20});
    auto wtxn = lmdb::test::beginWriteTransaction(env);
    auto dict = library::DictionaryStore{lmdb::test::openDatabase(wtxn, "dict"), wtxn};

    auto rockId = ao::test::requireValue(dict.put(wtxn, "rock"));
    auto jazzId = ao::test::requireValue(dict.put(wtxn, "jazz"));
    REQUIRE(wtxn.commit());

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
} // namespace ao::query::test
