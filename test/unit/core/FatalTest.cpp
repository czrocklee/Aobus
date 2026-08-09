// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/Contract.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace ao::test
{
  namespace
  {
    bool firstSink(FatalDiagnostic const& /*diagnostic*/)
    {
      return true;
    }
    bool secondSink(FatalDiagnostic const& /*diagnostic*/)
    {
      return false;
    }
  } // namespace

  TEST_CASE("Fatal - category names are stable", "[utility][unit][fatal]")
  {
    auto const categories = std::array{
      FatalCategory::Expects,
      FatalCategory::Ensures,
      FatalCategory::Invariant,
      FatalCategory::Fatal,
      FatalCategory::RealtimeInvariant,
      FatalCategory::UnhandledException,
    };
    auto const names = std::array{
      "expects",
      "ensures",
      "invariant",
      "fatal",
      "realtime-invariant",
      "unhandled-exception",
    };

    for (std::size_t index = 0; index < categories.size(); ++index)
    {
      CHECK(fatalCategoryName(categories[index]) == names[index]);
    }

    CHECK(fatalCategoryName(static_cast<FatalCategory>(255)) == "unknown");
  }

  TEST_CASE("Fatal - successful contracts are lazy", "[utility][unit][fatal]")
  {
    std::int32_t conditionEvaluations = 0;
    std::int32_t contextEvaluations = 0;

    AO_EXPECTS(++conditionEvaluations == 1, "unused context {}", ++contextEvaluations);
    AO_ENSURES(++conditionEvaluations == 2, "unused context {}", ++contextEvaluations);
    AO_INVARIANT(++conditionEvaluations == 3, "unused context {}", ++contextEvaluations);
    AO_RT_INVARIANT(++conditionEvaluations == 4, "unused realtime context");

    CHECK(conditionEvaluations == 4);
    CHECK(contextEvaluations == 0);
  }

  TEST_CASE("Fatal - sink registration preserves exact ownership", "[utility][unit][fatal]")
  {
    REQUIRE(registerFatalSink(&firstSink));
    CHECK_FALSE(registerFatalSink(&secondSink));
    CHECK_FALSE(unregisterFatalSink(&secondSink));
    CHECK(unregisterFatalSink(&firstSink));
    CHECK_FALSE(unregisterFatalSink(&firstSink));
    CHECK_FALSE(registerFatalSink(nullptr));
    CHECK_FALSE(unregisterFatalSink(nullptr));
  }

  TEST_CASE("Fatal - concurrent sink registration has one winner", "[utility][unit][fatal][concurrency]")
  {
    auto start = std::barrier{3};
    auto results = std::array<std::atomic_bool, 2>{};

    auto firstThread = std::jthread{[&]
                                    {
                                      start.arrive_and_wait();
                                      results[0].store(registerFatalSink(&firstSink), std::memory_order_relaxed);
                                    }};
    auto secondThread = std::jthread{[&]
                                     {
                                       start.arrive_and_wait();
                                       results[1].store(registerFatalSink(&secondSink), std::memory_order_relaxed);
                                     }};

    start.arrive_and_wait();
    firstThread.join();
    secondThread.join();

    auto const firstRegistered = results[0].load(std::memory_order_relaxed);
    auto const secondRegistered = results[1].load(std::memory_order_relaxed);
    CHECK(firstRegistered != secondRegistered);

    if (firstRegistered)
    {
      CHECK(unregisterFatalSink(&firstSink));
    }
    else
    {
      CHECK(unregisterFatalSink(&secondSink));
    }
  }
} // namespace ao::test
