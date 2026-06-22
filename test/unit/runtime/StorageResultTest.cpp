// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/rt/StorageResult.h>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <string_view>
#include <tuple>

namespace ao::rt::test
{
  TEST_CASE("StorageResult - NotFound maps to null optional", "[runtime][unit][storage-result]")
  {
    SECTION("returns the contained value")
    {
      auto const optResult = storageValueOrNullopt(Result<int>{42}, "load value");

      REQUIRE(optResult);
      CHECK(*optResult == 42);
    }

    SECTION("maps NotFound to nullopt")
    {
      auto const optResult = storageValueOrNullopt(
        Result<int>{std::unexpected{Error{.code = Error::Code::NotFound, .message = "missing value"}}}, "load value");

      CHECK(!optResult);
    }

    SECTION("throws for storage errors")
    {
      try
      {
        std::ignore = storageValueOrNullopt(
          Result<int>{std::unexpected{Error{.code = Error::Code::IoError, .message = "read failed"}}}, "load value");
        FAIL("storageValueOrNullopt should throw on non-NotFound errors");
      }
      catch (Exception const& e)
      {
        auto const message = std::string_view{e.what()};
        CHECK(message.find("load value") != std::string_view::npos);
        CHECK(message.find("read failed") != std::string_view::npos);
      }
    }
  }
} // namespace ao::rt::test
