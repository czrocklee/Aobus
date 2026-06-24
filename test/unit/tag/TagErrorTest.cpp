// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/tag/detail/TagError.h>

#include <catch2/catch_test_macros.hpp>

#include <source_location>
#include <string_view>

namespace ao::tag::test
{
  TEST_CASE("throwTagError(Error) preserves the original error's source location", "[tag][unit][error]")
  {
    auto const origin = std::source_location::current();
    auto const error = Error{.code = Error::Code::CorruptData, .message = "propagated failure", .location = origin};

    try
    {
      detail::throwTagError(error);
      FAIL("throwTagError was expected to throw");
    }
    catch (detail::TagException const& ex)
    {
      CHECK(ex.error().code == Error::Code::CorruptData);
      CHECK(std::string_view{ex.error().message} == "propagated failure");
      CHECK(ex.error().location.line() == origin.line());
      CHECK(std::string_view{ex.error().location.function_name()} == std::string_view{origin.function_name()});
    }
  }

  TEST_CASE("throwTagError(code, message) captures the call site", "[tag][unit][error]")
  {
    auto const here = std::source_location::current();

    try
    {
      detail::throwTagError(Error::Code::NotSupported, "fresh failure");
      FAIL("throwTagError was expected to throw");
    }
    catch (detail::TagException const& ex)
    {
      CHECK(ex.error().code == Error::Code::NotSupported);
      CHECK(std::string_view{ex.error().message} == "fresh failure");
      CHECK(std::string_view{ex.error().location.function_name()} == std::string_view{here.function_name()});
      CHECK(std::string_view{ex.error().location.file_name()} == std::string_view{here.file_name()});
      CHECK(ex.error().location.line() > here.line());
    }
  }

  TEST_CASE("TagException exposes its Error through what() and location()", "[tag][unit][error]")
  {
    auto const ex = detail::TagException{Error::Code::CorruptData, "corrupt tag data"};

    CHECK(ex.error().code == Error::Code::CorruptData);
    CHECK(std::string_view{ex.what()} == "corrupt tag data");
    CHECK(ex.location().line() == ex.error().location.line());
  }
} // namespace ao::tag::test
