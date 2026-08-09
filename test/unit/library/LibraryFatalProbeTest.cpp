// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/FatalProbeProcess.h"
#include "test/fatal/LibraryFatalProbeProtocol.h"
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
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_library_fatal_probe");
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
    }
  }
} // namespace ao::library::test
