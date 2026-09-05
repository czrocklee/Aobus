// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/ProbeProcess.h"
#include "test/fatal/TuiSignalProbeScenario.h"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>

namespace ao::tui::test
{
  TEST_CASE("TUI signal probe - watcher routes handleable signals outside the unit-test process",
            "[tui][integration][signal-exit][concurrency]")
  {
    constexpr auto kTimeout = std::chrono::seconds{15};
    auto const executablePath = ao::test::siblingProbeExecutablePath("ao_tui_signal_probe");
    REQUIRE_FALSE(executablePath.empty());

#ifdef _WIN32
    constexpr auto kScenarios = std::to_array<std::string_view>({"ctrl-c"});
#else
    constexpr auto kScenarios = std::to_array<std::string_view>({"sigint", "sigterm", "sighup", "restore-sigint"});
#endif

    for (auto const scenario : kScenarios)
    {
      INFO("probe: " << scenario);
      auto const result = ao::test::runProbeProcess(executablePath, scenario, kTimeout);

      REQUIRE(result.started);
      CHECK(result.launchError.empty());
      CHECK_FALSE(result.timedOut);

      if (result.exited && result.exitCode == static_cast<std::uint32_t>(kTuiSignalProbeSkipped))
      {
#ifdef _WIN32
        SKIP("CTRL_C_EVENT is not deliverable without an interactive console");
#else
        FAIL("TUI signal probe returned the Windows-only skip code");
#endif
      }

      REQUIRE(result.hasSuccessfulExit());
      CHECK((result.standardOutput.contains("routed") || result.standardOutput.contains("restored")));
    }
  }
} // namespace ao::tui::test
