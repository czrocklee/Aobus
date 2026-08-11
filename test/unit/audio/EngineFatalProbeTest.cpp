// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/AudioFatalProbeProtocol.h"
#include "test/fatal/ProbeProcess.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::audio::test
{
  TEST_CASE("Audio fatal invariants - subprocess probes abort with owned diagnostics",
            "[audio][integration][engine][concurrency]")
  {
    constexpr auto kTimeout = std::chrono::seconds{15};
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_audio_fatal_probe");
    REQUIRE_FALSE(executablePath.empty());

    for (auto const& expectation : audioFatalProbeExpectations())
    {
      INFO("probe: " << expectation.scenario);
      auto const result = ao::test::runProbeProcess(executablePath, expectation.scenario, kTimeout);

      REQUIRE(result.started);
      CHECK(result.launchError.empty());
      CHECK_FALSE(result.timedOut);
      CHECK(result.hasFatalTermination());
      CHECK(result.hasPlatformAbort());
      CHECK(result.standardError.contains("AOBUS_FATAL"));
      CHECK(result.standardError.contains(expectation.category));
      CHECK(result.standardError.contains(expectation.context));
      CHECK(result.standardError.contains("source="));
    }
  }
} // namespace ao::audio::test
