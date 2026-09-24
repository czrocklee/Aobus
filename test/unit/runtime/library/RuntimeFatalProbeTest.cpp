// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/ProbeProcess.h"
#include "test/fatal/RuntimeFatalProbeProtocol.h"
#include "test/unit/TestFixtureSupport.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace ao::rt::test
{
  namespace
  {
    ao::test::ProbeProcessResult runScenario(std::string_view scenario)
    {
      constexpr auto kTimeout = std::chrono::seconds{15};
      auto const executablePath = ao::test::currentProbeExecutablePath();
      REQUIRE_FALSE(executablePath.empty());
      auto scratch = ao::test::TempDir{};
      auto argument = std::string{scenario};
      argument.append(":").append(scratch.path().filename().string());
      return ao::test::runProbeProcess(executablePath, argument, kTimeout);
    }

    void requireFatalScenario(std::string_view scenario)
    {
      INFO("probe: " << scenario);
      auto const expectations = runtimeFatalProbeExpectations();
      auto const it = std::ranges::find(expectations, scenario, &RuntimeFatalProbeExpectation::scenario);
      REQUIRE(it != expectations.end());
      auto const& expectation = *it;
      auto const result = runScenario(scenario);
      REQUIRE(result.started);
      CHECK(result.launchError.empty());
      CHECK_FALSE(result.timedOut);
      CHECK(result.hasFatalTermination());
      CHECK(result.hasPlatformAbort());
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

    void requireCleanScenario(std::string_view scenario)
    {
      INFO("probe: " << scenario);
      REQUIRE(std::ranges::contains(runtimeCleanProbeScenarios(), scenario));
      auto const result = runScenario(scenario);
      REQUIRE(result.hasSuccessfulExit());
      CHECK_FALSE(result.standardError.contains("AOBUS_FATAL"));
    }
  } // namespace

  TEST_CASE("Runtime fatal roots - every registered probe scenario has its own case", "[runtime][unit][fatal]")
  {
    // Each registered scenario runs from its own case below, so a new registry
    // entry needs a matching case and an updated count here.
#ifdef _WIN32
    constexpr std::size_t kFatalScenarioCount = 38;
#else
    constexpr std::size_t kFatalScenarioCount = 36;
#endif
    CHECK(runtimeFatalProbeExpectations().size() == kFatalScenarioCount);
    CHECK(runtimeCleanProbeScenarios().size() == 2);
  }

  TEST_CASE("Runtime fatal roots - YAML export rejects a missing cover resource", "[runtime][integration][fatal]")
  {
    requireFatalScenario("yaml-export-missing-cover-resource");
  }

  TEST_CASE("Runtime fatal roots - projection source-order mismatch aborts instead of publishing a reset",
            "[runtime][integration][fatal]")
  {
    requireFatalScenario("projection-source-order-mismatch");
  }

  TEST_CASE("Runtime fatal roots - library publication admission exceptions abort", "[runtime][integration][fatal]")
  {
    requireFatalScenario("library-publication-admission-exception");
  }

  TEST_CASE("Runtime fatal roots - library replica exceptions abort", "[runtime][integration][fatal]")
  {
    requireFatalScenario("library-publication-replica-exception");
  }

  TEST_CASE("Runtime fatal roots - library observer exceptions abort", "[runtime][integration][fatal]")
  {
    requireFatalScenario("library-publication-observer-exception");
  }

  TEST_CASE("Runtime fatal roots - library completion exceptions abort", "[runtime][integration][fatal]")
  {
    requireFatalScenario("library-publication-completion-exception");
  }

  TEST_CASE("Runtime fatal roots - library completion acknowledgment must match publication",
            "[runtime][integration][fatal]")
  {
    requireFatalScenario("library-publication-completion-ack-invariant");
  }

  TEST_CASE("Runtime fatal roots - nested library delivery cannot destroy its owner", "[runtime][integration][fatal]")
  {
    requireFatalScenario("library-nested-delivery-destruction");
  }

  TEST_CASE("Runtime fatal roots - library mutation cannot execute after preview apply",
            "[runtime][integration][fatal]")
  {
    requireFatalScenario("library-mutation-execute-after-apply");
  }

  TEST_CASE("Runtime fatal roots - library mutation rejects a prestamped changeset", "[runtime][integration][fatal]")
  {
    requireFatalScenario("library-mutation-prestamped-changeset");
  }

  TEST_CASE("Runtime fatal roots - cancellation cannot rethrow an empty exception", "[runtime][integration][fatal]")
  {
    requireFatalScenario("operation-cancelled-empty-rethrow");
  }

  TEST_CASE("Runtime fatal roots - logged task exceptions abort", "[runtime][integration][fatal]")
  {
    requireFatalScenario("runtime-spawn-logged-exception");
  }

  TEST_CASE("Runtime fatal roots - cancellable task exceptions abort", "[runtime][integration][fatal]")
  {
    requireFatalScenario("runtime-cancellable-exception");
  }

  TEST_CASE("Runtime fatal roots - lifetime task exceptions retire the scope before abort",
            "[runtime][integration][fatal][concurrency]")
  {
    requireFatalScenario("runtime-lifetime-exception");
  }

  TEST_CASE("Runtime fatal roots - shutdown does not hide task exceptions",
            "[runtime][integration][fatal][concurrency]")
  {
    requireFatalScenario("runtime-shutdown-exception");
  }

  TEST_CASE("Runtime fatal roots - queued callback exceptions preserve queue bookkeeping",
            "[runtime][integration][fatal][async]")
  {
    requireFatalScenario("queued-executor-callback-exception");
  }

  TEST_CASE("Runtime fatal roots - final executor drain must reach quiescence",
            "[runtime][integration][fatal][concurrency]")
  {
    requireFatalScenario("queued-executor-final-drain-limit");
  }

  TEST_CASE("Runtime fatal roots - playback publication admission exceptions abort", "[runtime][integration][fatal]")
  {
    requireFatalScenario("playback-publication-admission-exception");
  }

  TEST_CASE("Runtime fatal roots - playback command drain admission exceptions abort", "[runtime][integration][fatal]")
  {
    requireFatalScenario("playback-command-drain-admission-exception");
  }

  TEST_CASE("Runtime fatal roots - playback command exceptions close playback before abort",
            "[runtime][integration][fatal]")
  {
    requireFatalScenario("playback-command-exception");
  }

  TEST_CASE("Runtime fatal roots - playing track reveal requires executor affinity",
            "[runtime][integration][fatal][concurrency]")
  {
    requireFatalScenario("playback-reveal-off-executor");
  }

  TEST_CASE("Runtime fatal roots - playback snapshot delivery cannot destroy its owner",
            "[runtime][integration][fatal]")
  {
    requireFatalScenario("playback-service-destroy-from-snapshot");
  }

  TEST_CASE("Runtime fatal roots - playback snapshot delivery cannot synchronously shut down",
            "[runtime][integration][fatal]")
  {
    requireFatalScenario("playback-service-shutdown-from-snapshot");
  }

  TEST_CASE("Runtime fatal roots - playback snapshot reads require executor affinity",
            "[runtime][integration][fatal][concurrency]")
  {
    requireFatalScenario("playback-service-snapshot-off-executor");
  }

  TEST_CASE("Runtime fatal roots - playback commands require executor affinity",
            "[runtime][integration][fatal][concurrency]")
  {
    requireFatalScenario("playback-service-command-off-executor");
  }

  TEST_CASE("Runtime fatal roots - playback subscriptions require executor affinity",
            "[runtime][integration][fatal][concurrency]")
  {
    requireFatalScenario("playback-service-event-off-executor");
  }

  TEST_CASE("Runtime fatal roots - runtime ordering changes require executor affinity",
            "[runtime][integration][fatal][concurrency]")
  {
    requireFatalScenario("runtime-ordering-off-executor");
  }

  TEST_CASE("Runtime fatal roots - view reads require executor affinity", "[runtime][integration][fatal][concurrency]")
  {
    requireFatalScenario("view-service-read-off-executor");
  }

  TEST_CASE("Runtime fatal roots - workspace observation admission exceptions abort", "[runtime][integration][fatal]")
  {
    requireFatalScenario("workspace-observation-admission-exception");
  }

  TEST_CASE("Runtime fatal roots - view projection observation admission exceptions abort",
            "[runtime][integration][fatal]")
  {
    requireFatalScenario("view-projection-observation-admission-exception");
  }

  TEST_CASE("Runtime fatal roots - view presentation observation admission exceptions abort",
            "[runtime][integration][fatal]")
  {
    requireFatalScenario("view-presentation-observation-admission-exception");
  }

  TEST_CASE("Runtime fatal roots - signal connection requires owner affinity",
            "[runtime][integration][fatal][concurrency]")
  {
    requireFatalScenario("signal-connect-off-owner");
  }

  TEST_CASE("Runtime fatal roots - signal disconnection requires owner affinity",
            "[runtime][integration][fatal][concurrency]")
  {
    requireFatalScenario("signal-disconnect-off-owner");
  }

  TEST_CASE("Runtime fatal roots - signal emission requires owner affinity",
            "[runtime][integration][fatal][concurrency]")
  {
    requireFatalScenario("signal-emit-off-owner");
  }

  TEST_CASE("Runtime fatal roots - signal observer exceptions abort", "[runtime][integration][fatal]")
  {
    requireFatalScenario("signal-observer-exception");
  }

  TEST_CASE("Runtime fatal roots - task future completion requires a result", "[runtime][integration][fatal]")
  {
    requireFatalScenario("task-future-missing-result");
  }

#ifdef _WIN32

  TEST_CASE("Runtime fatal roots - destructive restart release exceptions abort after successor launch",
            "[runtime][integration][fatal]")
  {
    requireFatalScenario("destructive-restart-release-exception");
  }

  TEST_CASE("Runtime fatal roots - destructive restart launch exceptions abort", "[runtime][integration][fatal]")
  {
    requireFatalScenario("destructive-restart-launch-exception");
  }
#endif

  TEST_CASE("Runtime shutdown cancellation - joined tasks retire their lifetime scope without fatal diagnostics",
            "[runtime][integration][fatal][async][concurrency]")
  {
    requireCleanScenario("runtime-shutdown-cancellation");
  }

  TEST_CASE(
    "Runtime shutdown cancellation - closing retires rejected library control delivery without fatal diagnostics",
    "[runtime][integration][fatal][async][concurrency]")
  {
    requireCleanScenario("library-control-delivery-closing-race");
  }
} // namespace ao::rt::test
