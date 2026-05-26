// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ao/Exception.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>

namespace ao::test
{
  TEST_CASE("Exception - Fatal exception infrastructure", "[core][unit][error]")
  {
    SECTION("ao::Exception tracks source location accurately")
    {
      try
      {
        // We throw it on a specific line to test __LINE__ macro expansion via source_location
        throwException<Exception>("Test invariant violation");
      }
      catch (Exception const& e)
      {
        CHECK(std::string{e.what()} == "Test invariant violation");
        CHECK_THAT(e.file(), Catch::Matchers::ContainsSubstring("ExceptionTest.cpp"));
        CHECK(e.line() > 0);
      }
    }

    SECTION("throwException with formatting")
    {
      try
      {
        throwException<Exception>("Test formatting: {} + {} = {}", 1, 2, 3);
      }
      catch (Exception const& e)
      {
        CHECK(std::string{e.what()} == "Test formatting: 1 + 2 = 3");
        CHECK_THAT(e.file(), Catch::Matchers::ContainsSubstring("ExceptionTest.cpp"));
        CHECK(e.line() > 0);
      }
    }
  }
} // namespace ao::test
