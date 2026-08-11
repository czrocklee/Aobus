// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/LibraryProbeProtocol.h"
#include "test/fatal/ProbeProcess.h"
#include "test/unit/TestFixtureSupport.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>

namespace ao::library::test
{
  TEST_CASE("Library prepared-write contracts - subprocess probes abort with owned diagnostics",
            "[library][integration][fatal]")
  {
    constexpr auto kTimeout = std::chrono::seconds{15};
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_library_probe");
    REQUIRE_FALSE(executablePath.empty());

    for (auto const& expectation : libraryFatalProbeExpectations())
    {
      INFO("probe: " << expectation.scenario);
      auto optScratch = std::optional<ao::test::TempDir>{};
      auto scenario = std::string{expectation.scenario};

      if (expectation.needsScratchDirectory)
      {
        optScratch.emplace();
        scenario.append(":").append(optScratch->path().filename().string());
      }

      auto const result = ao::test::runProbeProcess(executablePath, scenario, kTimeout);

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
    }
  }

  TEST_CASE("Library probe process - normal observation requires exact bounded standard output",
            "[library][integration][probe]")
  {
    constexpr auto kTimeout = std::chrono::seconds{15};
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_library_probe");
    REQUIRE_FALSE(executablePath.empty());

    for (auto const& expectation : libraryProbeObservationExpectations())
    {
      INFO("probe: " << expectation.scenario);
      auto optScratch = std::optional<ao::test::TempDir>{};
      auto scenario = std::string{expectation.scenario};

      if (expectation.needsScratchDirectory)
      {
        optScratch.emplace();
        scenario.append(":").append(optScratch->path().filename().string());
      }

      auto const result = ao::test::runProbeProcess(executablePath, scenario, kTimeout);

      REQUIRE(result.hasSuccessfulExit());
      CHECK_FALSE(result.hasFatalTermination());
      CHECK(result.standardOutput == expectation.standardOutput);
      CHECK(result.standardError.contains(expectation.standardErrorMarker));
      CHECK_FALSE(result.standardError.contains("AOBUS_FATAL"));
    }

    auto const oversizedResult = ao::test::runProbeProcess(executablePath, "oversized-standard-output", kTimeout);
    REQUIRE(oversizedResult.hasSuccessfulExit());
    CHECK(oversizedResult.standardOutput == std::string(ao::test::kMaximumProbeOutputBytes, 'x'));
    CHECK(oversizedResult.standardError.empty());
  }
} // namespace ao::library::test
