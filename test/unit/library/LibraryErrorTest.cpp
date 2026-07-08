// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/library/detail/LibraryError.h>

#include <catch2/catch_test_macros.hpp>

#include <source_location>
#include <string_view>

namespace ao::library::test
{
  TEST_CASE("throwLibraryError - preserves the original error's source location", "[library][unit][error]")
  {
    auto const origin = std::source_location::current();
    auto const error = Error{.code = Error::Code::IoError, .message = "propagated failure", .location = origin};

    try
    {
      detail::throwLibraryError(error);
      FAIL("throwLibraryError was expected to throw");
    }
    catch (detail::LibraryException const& ex)
    {
      CHECK(ex.error().code == Error::Code::IoError);
      CHECK(std::string_view{ex.error().message} == "propagated failure");
      CHECK(ex.error().location.line() == origin.line());
      CHECK(std::string_view{ex.error().location.function_name()} == std::string_view{origin.function_name()});
    }
  }

  TEST_CASE("throwLibraryError - captures the call site", "[library][unit][error]")
  {
    auto const here = std::source_location::current();

    try
    {
      detail::throwLibraryError(Error::Code::ValueTooLarge, "fresh failure");
      FAIL("throwLibraryError was expected to throw");
    }
    catch (detail::LibraryException const& ex)
    {
      CHECK(ex.error().code == Error::Code::ValueTooLarge);
      CHECK(std::string_view{ex.error().message} == "fresh failure");
      CHECK(std::string_view{ex.error().location.function_name()} == std::string_view{here.function_name()});
      CHECK(std::string_view{ex.error().location.file_name()} == std::string_view{here.file_name()});
      CHECK(ex.error().location.line() > here.line());
    }
  }

  TEST_CASE("LibraryException - exposes its Error through what() and location()", "[library][unit][error]")
  {
    auto const ex = detail::LibraryException{Error::Code::ResourceExhausted, "store full"};

    CHECK(ex.error().code == Error::Code::ResourceExhausted);
    CHECK(std::string_view{ex.what()} == "store full");
    CHECK(ex.location().line() == ex.error().location.line());
  }
} // namespace ao::library::test
