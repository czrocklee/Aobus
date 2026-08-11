// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/LibraryProbeProtocol.h"
#include "test/fatal/LibraryProbeScenario.h"
#include "test/fatal/ProbeProcess.h"
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
                   ao::test::ProbeProcessResult const& result)
  {
    auto const conditionMatches =
      expectation.condition.empty() || result.standardError.contains("condition=" + std::string{expectation.condition});
    return result.hasPlatformAbort() && result.standardError.contains("AOBUS_FATAL") &&
           result.standardError.contains("category=" + std::string{expectation.category}) && conditionMatches &&
           result.standardError.contains(expectation.context) && result.standardError.contains(expectation.source) &&
           result.standardError.contains(expectation.function);
  }

  bool verifyProbe(ao::library::test::LibraryProbeObservationExpectation const& expectation,
                   ao::test::ProbeProcessResult const& result)
  {
    auto const diagnosticMatches =
      expectation.standardErrorMarker.empty() || result.standardError.contains(expectation.standardErrorMarker);
    return result.hasSuccessfulExit() && result.standardOutput == expectation.standardOutput && diagnosticMatches &&
           !result.standardError.contains("AOBUS_FATAL");
  }
} // namespace

int main(int argc, char* argv[])
{
  if (argc == 3 && std::string_view{argv[1]} == "--aobus-probe-child")
  {
    return ao::library::test::runLibraryProbeScenario(argv[2]);
  }

  if (argc != 1)
  {
    std::println(stderr, "Usage: ao_library_probe [--aobus-probe-child <scenario>]");
    return 2;
  }

  auto const executablePath = ao::test::currentProbeExecutablePath();

  if (executablePath.empty())
  {
    std::println(stderr, "ao_library_probe could not resolve its executable path");
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

    auto const result = ao::test::runProbeProcess(executablePath, scenario, kProbeTimeout);

    if (!verifyProbe(expectation, result))
    {
      std::println(stderr,
                   "ao_library_probe fatal scenario '{}' failed: started={} timed-out={} exited={} "
                   "exit-code={} signaled={} signal={} launch-error={}\nstdout:\n{}\nstderr:\n{}",
                   expectation.scenario,
                   result.started,
                   result.timedOut,
                   result.exited,
                   result.exitCode,
                   result.signaled,
                   result.signalNumber,
                   result.launchError,
                   result.standardOutput,
                   result.standardError);
      return 1;
    }
  }

  for (auto const& expectation : ao::library::test::libraryProbeObservationExpectations())
  {
    auto optScratch = std::optional<ao::test::TempDir>{};
    auto scenario = std::string{expectation.scenario};

    if (expectation.needsScratchDirectory)
    {
      optScratch.emplace();
      scenario.append(":").append(optScratch->path().filename().string());
    }

    auto const result = ao::test::runProbeProcess(executablePath, scenario, kProbeTimeout);

    if (!verifyProbe(expectation, result))
    {
      std::println(stderr,
                   "ao_library_probe observation scenario '{}' failed: started={} timed-out={} exited={} "
                   "exit-code={} signaled={} signal={} launch-error={}\nstdout:\n{}\nstderr:\n{}",
                   expectation.scenario,
                   result.started,
                   result.timedOut,
                   result.exited,
                   result.exitCode,
                   result.signaled,
                   result.signalNumber,
                   result.launchError,
                   result.standardOutput,
                   result.standardError);
      return 1;
    }
  }

  return 0;
}
