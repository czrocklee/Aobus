// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/FatalProbeProcess.h"
#include "test/fatal/LibraryFatalProbeProtocol.h"
#include "test/fatal/LibraryFatalProbeScenario.h"
#include "test/unit/TestFixtureSupport.h"

#include <chrono>
#include <optional>
#include <print>
#include <string>
#include <string_view>

namespace
{
  constexpr auto kProbeTimeout = std::chrono::seconds{15};

  bool verifyProbe(ao::library::test::LibraryFatalProbeExpectation const& expectation,
                   ao::test::FatalProbeResult const& result)
  {
    auto const conditionMatches =
      expectation.condition.empty() || result.standardError.contains("condition=" + std::string{expectation.condition});
    return result.started && result.launchError.empty() && !result.timedOut && result.endedByPlatformAbort() &&
           result.standardError.contains("AOBUS_FATAL") &&
           result.standardError.contains("category=" + std::string{expectation.category}) && conditionMatches &&
           result.standardError.contains(expectation.context) && result.standardError.contains(expectation.source) &&
           result.standardError.contains(expectation.function);
  }
} // namespace

int main(int argc, char* argv[])
{
  if (argc == 3 && std::string_view{argv[1]} == "--aobus-fatal-probe-child")
  {
    return ao::library::test::runLibraryFatalProbeScenario(argv[2]);
  }

  if (argc != 1)
  {
    std::println(stderr, "Usage: ao_library_fatal_probe [--aobus-fatal-probe-child <scenario>]");
    return 2;
  }

  auto const executablePath = ao::test::currentProbeExecutablePath();

  if (executablePath.empty())
  {
    std::println(stderr, "ao_library_fatal_probe could not resolve its executable path");
    return 1;
  }

  for (auto const& expectation : ao::library::test::libraryFatalProbeExpectations())
  {
    auto optScratch = std::optional<ao::test::TempDir>{};
    auto scenario = std::string{expectation.scenario};

    if (expectation.needsScratchDirectory)
    {
      optScratch.emplace();
      scenario.append(":").append(optScratch->path().filename().string());
    }

    auto const result = ao::test::runFatalProbe(executablePath, scenario, kProbeTimeout);

    if (!verifyProbe(expectation, result))
    {
      std::println(stderr,
                   "ao_library_fatal_probe scenario '{}' failed: started={} timed-out={} exited={} "
                   "exit-code={} signaled={} signal={} launch-error={}\nstderr:\n{}",
                   expectation.scenario,
                   result.started,
                   result.timedOut,
                   result.exited,
                   result.exitCode,
                   result.signaled,
                   result.signalNumber,
                   result.launchError,
                   result.standardError);
      return 1;
    }
  }

  return 0;
}
