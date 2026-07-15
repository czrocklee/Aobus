// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/MainContextCallbackScope.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>

namespace ao::gtk::test
{
  TEST_CASE("MainContextCallbackScope - guarded callbacks run only while the scope is open",
            "[gtk][unit][callback-lifetime]")
  {
    auto callback = std::function<void(std::int32_t)>{};
    std::int32_t observedValue = 0;

    {
      auto scope = MainContextCallbackScope{};
      callback = scope.guard([&observedValue](std::int32_t value) { observedValue += value; });

      callback(2);

      CHECK(observedValue == 2);
    }

    callback(5);

    CHECK(observedValue == 2);
  }

  TEST_CASE("MainContextCallbackScope - close invalidates callbacks before teardown and is idempotent",
            "[gtk][regression][callback-lifetime][concurrency]")
  {
    auto callback = std::function<void()>{};
    std::int32_t callbackCount = 0;
    std::int32_t closeCount = 0;

    {
      auto scope = MainContextCallbackScope{[&]
                                            {
                                              ++closeCount;
                                              callback();
                                            }};
      callback = scope.guard([&callbackCount] { ++callbackCount; });

      scope.close();
      scope.close();

      CHECK(callbackCount == 0);
      CHECK(closeCount == 1);
    }

    CHECK(closeCount == 1);
  }
} // namespace ao::gtk::test
