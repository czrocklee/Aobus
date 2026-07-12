// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/query/Completion.h>
#include <ao/rt/Log.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ao::query::test
{
  // Completion runs on every keystroke, so its per-call latency is a user-facing
  // metric. This logs the average cost of analyzeQueryCompletion over a mix of
  // inputs (including the completed-predicate path that drives logical-operator
  // suggestions). Log-only baseline - no fixed threshold (machine dependent).
  TEST_CASE("Completion - records latency baseline", "[query][unit][completion][baseline]")
  {
    rt::Log::initialize(rt::LogLevel::Info);

    auto const inputs = std::array<std::string_view, 4>{
      R"($artist = "Miles Davis" )",   // completed predicate -> logical-operator suggestion path
      R"($artist = "Miles Davis" an)", // partial logical operator
      "$ar",                           // variable completion
      R"($album = "Kind" and $ye)",    // second predicate, variable completion
    };

    constexpr int kIterations = 20000;
    std::size_t sink = 0;

    auto const start = std::chrono::steady_clock::now();

    for (std::int32_t i = 0; i < kIterations; ++i)
    {
      for (auto const input : inputs)
      {
        if (analyzeQueryCompletion(input, input.size()))
        {
          ++sink;
        }
      }
    }

    auto const elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
    auto const calls = static_cast<std::int64_t>(kIterations) * static_cast<std::int64_t>(inputs.size());

    APP_LOG_INFO("=== Completion latency: {} calls, {} ns/call ===", calls, elapsed.count() / calls);

    // Sanity: the representative inputs must keep producing completion contexts.
    CHECK(sink > 0);

    rt::Log::shutdown();
  }
} // namespace ao::query::test
