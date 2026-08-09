// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/FatalProbeProcess.h"
#include "test/fatal/RuntimeFatalProbeProtocol.h"
#include "test/unit/TestFixtureSupport.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

namespace ao::rt::test
{
  TEST_CASE("Runtime fatal roots - subprocess probes abort with owned diagnostics", "[runtime][integration][fatal]")
  {
    constexpr auto kTimeout = std::chrono::seconds{15};
    auto const executablePath = ao::test::currentProbeExecutablePath();
    REQUIRE_FALSE(executablePath.empty());

    for (auto const& expectation : runtimeFatalProbeExpectations())
    {
      INFO("probe: " << expectation.scenario);
      auto scratch = ao::test::TempDir{};
      auto scenario = std::string{expectation.scenario};
      scenario.append(":").append(scratch.path().filename().string());
      auto const result = ao::test::runFatalProbe(executablePath, scenario, kTimeout);

      REQUIRE(result.started);
      CHECK(result.launchError.empty());
      CHECK_FALSE(result.timedOut);
      CHECK(result.endedByFatalTermination());
      CHECK(result.endedByPlatformAbort());
      CHECK(result.standardError.contains("AOBUS_FATAL"));
      CHECK(result.standardError.contains("category=" + std::string{expectation.category}));

      if (!expectation.condition.empty())
      {
        CHECK(result.standardError.contains("condition=" + std::string{expectation.condition}));
      }

      CHECK(result.standardError.contains(expectation.context));
      CHECK(result.standardError.contains(expectation.source));
      CHECK(result.standardError.contains(expectation.function));

      if (!expectation.marker.empty())
      {
        CHECK(result.standardError.contains(expectation.marker));
      }

      if (!expectation.secondMarker.empty())
      {
        CHECK(result.standardError.contains(expectation.secondMarker));
      }
    }
  }

  TEST_CASE("Runtime shutdown cancellation - subprocess exits without fatal diagnostics",
            "[runtime][integration][async][concurrency]")
  {
    constexpr auto kTimeout = std::chrono::seconds{15};
    auto const executablePath = ao::test::currentProbeExecutablePath();
    REQUIRE_FALSE(executablePath.empty());

    for (auto const scenario : runtimeCleanProbeScenarios())
    {
      INFO("probe: " << scenario);
      auto const result = ao::test::runFatalProbe(executablePath, scenario, kTimeout);

      REQUIRE(result.started);
      CHECK(result.launchError.empty());
      CHECK_FALSE(result.timedOut);
      CHECK(result.exited);
      CHECK(result.exitCode == 0);
      CHECK_FALSE(result.signaled);
      CHECK_FALSE(result.standardError.contains("AOBUS_FATAL"));
    }
  }
} // namespace ao::rt::test
