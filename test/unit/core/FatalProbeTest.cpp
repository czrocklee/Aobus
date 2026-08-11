// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/FatalProbeProtocol.h"
#include "test/fatal/ProbeProcess.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string_view>

namespace ao::test
{
  namespace
  {
    constexpr auto kProbeTimeout = std::chrono::seconds{15};
  } // namespace

  TEST_CASE("Fatal - subprocess probes preserve diagnostics and abort", "[utility][integration][fatal][concurrency]")
  {
    auto const probeExecutablePath = fatalProbeExecutablePath();
    REQUIRE_FALSE(probeExecutablePath.empty());

    for (auto const& expectation : fatalProbeExpectations())
    {
      INFO("probe: " << expectation.scenario);
      auto const result = runProbeProcess(probeExecutablePath, expectation.scenario, kProbeTimeout);

      REQUIRE(result.started);
      CHECK(result.launchError.empty());
      CHECK_FALSE(result.timedOut);
      CHECK(result.hasFatalTermination());
      CHECK(result.hasPlatformAbort());
      CHECK(result.standardError.contains("AOBUS_FATAL"));
      CHECK(result.standardError.contains(expectation.requiredMarker));
      CHECK(result.standardError.contains(expectation.secondRequiredMarker));
      CHECK(result.standardError.contains(expectation.sourceMarker));

      if (!expectation.thirdRequiredMarker.empty())
      {
        CHECK(result.standardError.contains(expectation.thirdRequiredMarker));
      }

      if (expectation.scenario == "truncated-diagnostic")
      {
        CHECK(result.standardError.ends_with('\n'));
      }

      if (!expectation.forbiddenMarker.empty())
      {
        CHECK_FALSE(result.standardError.contains(expectation.forbiddenMarker));
      }

      if (expectation.emergencyBeforeSink)
      {
        CHECK(result.standardError.find("AOBUS_FATAL category=fatal") <
              result.standardError.find("AOBUS_TEST sink=accepted"));
      }
    }
  }
} // namespace ao::test
