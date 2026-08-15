// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/lmdb/detail/ResultError.h"

#include <ao/Error.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <cerrno>
#include <source_location>
#include <string_view>

namespace ao::lmdb::test
{
  TEST_CASE("ResultError - maps LMDB codes and captures the caller location", "[lmdb][unit][error]")
  {
    SECTION("Success code yields a value")
    {
      CHECK(resultFromCode("mdb_noop", MDB_SUCCESS).has_value());
    }

    SECTION("resultFromCode points at the caller, not the resultFromCode/lmdbError wrappers")
    {
      auto const expectedLine = std::source_location::current().line() + 1;
      auto const result = resultFromCode("mdb_get", MDB_NOTFOUND);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::NotFound);
      CHECK(result.error().location.line() == expectedLine);
      CHECK(std::string_view{result.error().location.file_name()}.ends_with("ResultErrorTest.cpp"));

      auto const fn = std::string_view{result.error().location.function_name()};
      CHECK_FALSE(fn.contains("resultFromCode"));
      CHECK_FALSE(fn.contains("lmdbError"));
    }

    SECTION("lmdbError captures the caller and classifies the code")
    {
      auto const expectedLine = std::source_location::current().line() + 1;
      auto const errorRes = Result<>{lmdbError("mdb_put", MDB_KEYEXIST)};

      REQUIRE_FALSE(errorRes);
      CHECK(errorRes.error().code == Error::Code::Conflict);
      CHECK(errorRes.error().location.line() == expectedLine);
      CHECK(std::string_view{errorRes.error().location.file_name()}.ends_with("ResultErrorTest.cpp"));
    }

    SECTION("An exhausted map is its own code, separate from other storage failures")
    {
      // A caller may answer this one by reopening with a larger map, so it must
      // not arrive wearing the code of a failure that repeating would not fix.
      CHECK(resultFromCode("mdb_put", MDB_MAP_FULL).error().code == Error::Code::StorageFull);
      CHECK(resultFromCode("mdb_put", ENOSPC).error().code == Error::Code::IoError);
      CHECK(resultFromCode("mdb_cursor_put", MDB_TXN_FULL).error().code == Error::Code::IoError);
    }

    SECTION("A map another process outgrew reports stale state")
    {
      CHECK(resultFromCode("mdb_txn_begin", MDB_MAP_RESIZED).error().code == Error::Code::InvalidState);
    }

    SECTION("Unmapped failures collapse to IoError")
    {
      CHECK(resultFromCode("mdb_get", MDB_CORRUPTED).error().code == Error::Code::IoError);
      CHECK(resultFromCode("mdb_txn_begin", MDB_READERS_FULL).error().code == Error::Code::IoError);
    }
  }
} // namespace ao::lmdb::test
