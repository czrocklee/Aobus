// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "lib/lmdb/detail/ResultError.h"

#include <ao/Error.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

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
      auto const error = Result<>{lmdbError("mdb_put", MDB_KEYEXIST)};

      REQUIRE_FALSE(error);
      CHECK(error.error().code == Error::Code::Conflict);
      CHECK(error.error().location.line() == expectedLine);
      CHECK(std::string_view{error.error().location.file_name()}.ends_with("ResultErrorTest.cpp"));
    }

    SECTION("Unmapped failures collapse to IoError")
    {
      CHECK(resultFromCode("mdb_get", MDB_CORRUPTED).error().code == Error::Code::IoError);
    }
  }
} // namespace ao::lmdb::test
