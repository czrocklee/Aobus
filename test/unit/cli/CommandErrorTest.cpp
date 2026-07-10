// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CommandError.h"

#include <ao/CoreIds.h>
#include <ao/Error.h>

#include <catch2/catch_test_macros.hpp>

#include <source_location>
#include <string_view>

namespace ao::cli::test
{
  TEST_CASE("throwCommandError - preserves the original error", "[cli][unit][error]")
  {
    auto const origin = std::source_location::current();
    auto const error = Error{.code = Error::Code::IoError, .message = "propagated failure", .location = origin};

    try
    {
      throwCommandError(error);
    }
    catch (CommandError const& ex)
    {
      CHECK(ex.error().code == Error::Code::IoError);
      CHECK(std::string_view{ex.error().message} == "propagated failure");
      CHECK(ex.error().location.line() == origin.line());
      CHECK(std::string_view{ex.error().location.function_name()} == std::string_view{origin.function_name()});
      CHECK(ex.code() == Error::Code::IoError);
      CHECK(std::string_view{ex.what()} == "propagated failure");
    }
  }

  TEST_CASE("throwCommandError - preserves code and source location", "[cli][unit][error]")
  {
    auto const origin = std::source_location::current();
    auto const error = Error{.code = Error::Code::IoError, .message = "disk full", .location = origin};

    try
    {
      throwCommandError(error, "export failed: {}", error.message);
    }
    catch (CommandError const& ex)
    {
      CHECK(ex.error().code == Error::Code::IoError);
      CHECK(std::string_view{ex.error().message} == "export failed: disk full");
      CHECK(ex.error().location.line() == origin.line());
      CHECK(std::string_view{ex.error().location.function_name()} == std::string_view{origin.function_name()});
    }
  }

  TEST_CASE("throwCommandError - captures the call site", "[cli][unit][error]")
  {
    auto const here = std::source_location::current();

    try
    {
      throwCommandError(Error::Code::InvalidInput, "fresh failure");
    }
    catch (CommandError const& ex)
    {
      CHECK(ex.error().code == Error::Code::InvalidInput);
      CHECK(std::string_view{ex.error().message} == "fresh failure");
      CHECK(std::string_view{ex.error().location.function_name()} == std::string_view{here.function_name()});
      CHECK(std::string_view{ex.error().location.file_name()} == std::string_view{here.file_name()});
      CHECK(ex.error().location.line() > here.line());
    }
  }

  TEST_CASE("throwCommandError format overloads format inline and preserve codes", "[cli][unit][error]")
  {
    SECTION("not found code")
    {
      auto const here = std::source_location::current();

      try
      {
        throwCommandError(Error::Code::NotFound, "track not found: {}", TrackId{42});
      }
      catch (CommandError const& ex)
      {
        CHECK(ex.error().code == Error::Code::NotFound);
        CHECK(std::string_view{ex.error().message} == "track not found: 42");
        CHECK(std::string_view{ex.error().location.function_name()} == std::string_view{here.function_name()});
        CHECK(ex.error().location.line() > here.line());
      }
    }

    SECTION("explicit code")
    {
      auto const here = std::source_location::current();

      try
      {
        throwCommandError(Error::Code::FormatRejected, "filter error: {}", "bad token");
      }
      catch (CommandError const& ex)
      {
        CHECK(ex.error().code == Error::Code::FormatRejected);
        CHECK(std::string_view{ex.error().message} == "filter error: bad token");
        CHECK(std::string_view{ex.error().location.function_name()} == std::string_view{here.function_name()});
        CHECK(ex.error().location.line() > here.line());
      }
    }
  }
} // namespace ao::cli::test
