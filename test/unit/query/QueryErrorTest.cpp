// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/query/detail/QueryError.h>

#include <catch2/catch_test_macros.hpp>

#include <source_location>
#include <string_view>

namespace ao::query::test
{
  TEST_CASE("throwQueryError - preserves the original error's source location", "[query][unit][error]")
  {
    auto const origin = std::source_location::current();
    auto const error = Error{.code = Error::Code::FormatRejected, .message = "propagated failure", .location = origin};

    try
    {
      detail::throwQueryError(error);
    }
    catch (detail::QueryException const& ex)
    {
      CHECK(ex.error().code == Error::Code::FormatRejected);
      CHECK(std::string_view{ex.error().message} == "propagated failure");
      CHECK(ex.error().location.line() == origin.line());
      CHECK(std::string_view{ex.error().location.function_name()} == std::string_view{origin.function_name()});
    }
  }

  TEST_CASE("throwQueryError - captures the call site with FormatRejected", "[query][unit][error]")
  {
    auto const here = std::source_location::current();

    try
    {
      detail::throwQueryError("fresh failure");
    }
    catch (detail::QueryException const& ex)
    {
      CHECK(ex.error().code == Error::Code::FormatRejected);
      CHECK(std::string_view{ex.error().message} == "fresh failure");
      CHECK(std::string_view{ex.error().location.function_name()} == std::string_view{here.function_name()});
      CHECK(std::string_view{ex.error().location.file_name()} == std::string_view{here.file_name()});
      CHECK(ex.error().location.line() > here.line());
    }
  }

  TEST_CASE("throwQueryError - formats inline and records the call site", "[query][unit][error]")
  {
    auto const here = std::source_location::current();

    try
    {
      detail::throwQueryError("field '{}' is unsupported for field {}", "duration", "Duration");
    }
    catch (detail::QueryException const& ex)
    {
      CHECK(ex.error().code == Error::Code::FormatRejected);
      CHECK(std::string_view{ex.error().message} == "field 'duration' is unsupported for field Duration");
      CHECK(std::string_view{ex.error().location.function_name()} == std::string_view{here.function_name()});
      CHECK(ex.error().location.line() > here.line());
    }
  }

  TEST_CASE("QueryException - exposes its Error through what() and location()", "[query][unit][error]")
  {
    auto const ex = detail::QueryException{Error::Code::FormatRejected, "bad query expression"};

    CHECK(ex.error().code == Error::Code::FormatRejected);
    CHECK(std::string_view{ex.what()} == "bad query expression");
    CHECK(ex.location().line() == ex.error().location.line());
  }
} // namespace ao::query::test
