// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>

#include <catch2/catch_test_macros.hpp>

#include <source_location>
#include <string>
#include <string_view>

namespace ao::test
{
  TEST_CASE("Error - Recoverable error infrastructure", "[core][unit][error]")
  {
    SECTION("makeError produces correct Error payload")
    {
      auto const result = Result{makeError(Error::Code::NotFound, "Item not in database")};

      REQUIRE_FALSE(result.has_value());
      CHECK(result.error().code == Error::Code::NotFound);
      CHECK(result.error().message == "Item not in database");
    }

    SECTION("Result<T> integration with makeError")
    {
      auto fn = [] -> Result<int> { return makeError(Error::Code::InvalidState, "Bad state"); };

      auto const res = fn();
      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidState);
      CHECK(res.error().message == "Bad state");
    }

    SECTION("Error codes cover external data and storage failures")
    {
      auto const invalidInput = Result{makeError(Error::Code::InvalidInput, "Invalid user value")};
      auto const corruptData = Result{makeError(Error::Code::CorruptData, "Corrupt file")};
      auto const conflict = Result{makeError(Error::Code::Conflict, "Record already exists")};
      auto const tooLarge = Result{makeError(Error::Code::ValueTooLarge, "Serialized record is too large")};
      auto const resourceExhausted = Result{makeError(Error::Code::ResourceExhausted, "Resource IDs exhausted")};

      REQUIRE_FALSE(invalidInput);
      REQUIRE_FALSE(corruptData);
      REQUIRE_FALSE(conflict);
      REQUIRE_FALSE(tooLarge);
      REQUIRE_FALSE(resourceExhausted);

      CHECK(invalidInput.error().code == Error::Code::InvalidInput);
      CHECK(corruptData.error().code == Error::Code::CorruptData);
      CHECK(conflict.error().code == Error::Code::Conflict);
      CHECK(tooLarge.error().code == Error::Code::ValueTooLarge);
      CHECK(resourceExhausted.error().code == Error::Code::ResourceExhausted);
    }

    SECTION("makeError captures the caller's source location, not makeError's body")
    {
      auto const expectedLine = std::source_location::current().line() + 1;
      auto const result = Result{makeError(Error::Code::IoError, "disk gone")};

      auto const& loc = result.error().location;
      CHECK(loc.line() == expectedLine);
      CHECK(std::string_view{loc.file_name()}.ends_with("ErrorTest.cpp"));
      CHECK(std::string_view{loc.function_name()}.find("makeError") == std::string_view::npos);
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
