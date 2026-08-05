// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>

namespace ao::test
{
  TEST_CASE("Error - recoverable error infrastructure", "[core][unit][error]")
  {
    SECTION("makeError produces correct Error payload")
    {
      auto const result = Result<>{makeError(Error::Code::NotFound, "Item not in database")};

      REQUIRE_FALSE(result.has_value());
      CHECK(result.error().code == Error::Code::NotFound);
      CHECK(result.error().message == "Item not in database");
    }

    SECTION("Result<T> integration with makeError")
    {
      auto fn = [] -> Result<int> { return makeError(Error::Code::InvalidState, "Bad state"); };

      auto const resRes = fn();
      REQUIRE_FALSE(resRes);
      CHECK(resRes.error().code == Error::Code::InvalidState);
      CHECK(resRes.error().message == "Bad state");
    }

    SECTION("Result<T> remains expected-compatible")
    {
      STATIC_REQUIRE(std::is_base_of_v<std::expected<int, Error>, Result<int>>);
      STATIC_REQUIRE_FALSE(std::is_same_v<std::expected<int, Error>, Result<int>>);

      auto const fromBaseRes = Result<int>{std::expected<int, Error>{7}};
      REQUIRE(fromBaseRes);
      CHECK(*fromBaseRes == 7);

      auto const fromUnexpectedRes = Result<int>{makeError(Error::Code::InvalidInput, "bad value")};
      REQUIRE_FALSE(fromUnexpectedRes);
      CHECK(fromUnexpectedRes.error().code == Error::Code::InvalidInput);
    }

    SECTION("Result<> supports void success and errors")
    {
      auto const successRes = Result<>{std::expected<void, Error>{}};
      CHECK(successRes);

      auto const failureRes = Result<>{makeError(Error::Code::IoError, "disk gone")};
      REQUIRE_FALSE(failureRes);
      CHECK(failureRes.error().message == "disk gone");
    }

    SECTION("Error codes cover external data and storage failures")
    {
      auto const invalidInputRes = Result<>{makeError(Error::Code::InvalidInput, "Invalid user value")};
      auto const corruptDataRes = Result<>{makeError(Error::Code::CorruptData, "Corrupt file")};
      auto const conflictRes = Result<>{makeError(Error::Code::Conflict, "Record already exists")};
      auto const resourceBusyRes = Result<>{makeError(Error::Code::ResourceBusy, "Audio device is busy")};
      auto const tooLargeRes = Result<>{makeError(Error::Code::ValueTooLarge, "Serialized record is too large")};
      auto const resourceExhaustedRes = Result<>{makeError(Error::Code::ResourceExhausted, "Resource IDs exhausted")};

      REQUIRE_FALSE(invalidInputRes);
      REQUIRE_FALSE(corruptDataRes);
      REQUIRE_FALSE(conflictRes);
      REQUIRE_FALSE(resourceBusyRes);
      REQUIRE_FALSE(tooLargeRes);
      REQUIRE_FALSE(resourceExhaustedRes);

      CHECK(invalidInputRes.error().code == Error::Code::InvalidInput);
      CHECK(corruptDataRes.error().code == Error::Code::CorruptData);
      CHECK(conflictRes.error().code == Error::Code::Conflict);
      CHECK(resourceBusyRes.error().code == Error::Code::ResourceBusy);
      CHECK(tooLargeRes.error().code == Error::Code::ValueTooLarge);
      CHECK(resourceExhaustedRes.error().code == Error::Code::ResourceExhausted);
    }

    SECTION("makeError captures the caller's source location, not makeError's body")
    {
      auto const expectedLine = std::source_location::current().line() + 1;
      auto const result = Result<>{makeError(Error::Code::IoError, "disk gone")};

      auto const& loc = result.error().location;
      CHECK(loc.line() == expectedLine);
      CHECK(std::string_view{loc.file_name()}.ends_with("ErrorTest.cpp"));
      CHECK_FALSE(std::string_view{loc.function_name()}.contains("makeError"));
    }

    SECTION("Direct aggregate Error captures its own construction site")
    {
      auto const expectedLine = std::source_location::current().line() + 1;
      auto const error = Error{.code = Error::Code::InvalidState, .message = "bad"};

      CHECK(error.location.line() == expectedLine);
      CHECK(std::string_view{error.location.file_name()}.ends_with("ErrorTest.cpp"));
    }
  }
} // namespace ao::test
