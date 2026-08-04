// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/WinUiErrorBoundary.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>

namespace ao::winui::test
{
  TEST_CASE("runOptionalWinRt - executes a successful operation", "[winui][unit][error-boundary]")
  {
    bool called = false;

    runOptionalWinRt("test operation", [&called] { called = true; });

    CHECK(called);
  }

  TEST_CASE("runOptionalWinRt - contains a WinRT failure", "[winui][unit][error-boundary]")
  {
    bool called = false;
    constexpr auto kFailure = static_cast<std::int32_t>(0x80004005U);

    CHECK_NOTHROW(runOptionalWinRt("test operation",
                                   [&called]
                                   {
                                     called = true;
                                     throw winrt::hresult_error{winrt::hresult{kFailure}};
                                   }));

    CHECK(called);
  }

  TEST_CASE("runOptionalWinRt - does not hide a C++ failure", "[winui][unit][error-boundary]")
  {
    CHECK_THROWS_AS(
      runOptionalWinRt("test operation", [] { throw std::runtime_error{"logic failure"}; }), std::runtime_error);
  }
} // namespace ao::winui::test
