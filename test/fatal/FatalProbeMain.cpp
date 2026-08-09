// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/FatalProbeProcess.h"
#include "test/fatal/FatalProbeProtocol.h"
#include "test/fatal/FatalProbeScenario.h"

#include <chrono>
#include <print>
#include <string_view>

namespace
{
  constexpr auto kProbeTimeout = std::chrono::seconds{15};

  bool verifyProbe(ao::test::FatalProbeExpectation const& expectation, ao::test::FatalProbeResult const& result)
  {
    auto passed = result.started && result.launchError.empty() && !result.timedOut && result.endedByPlatformAbort() &&
                  result.standardError.contains("AOBUS_FATAL") &&
                  result.standardError.contains(expectation.requiredMarker) &&
                  result.standardError.contains(expectation.secondRequiredMarker) &&
                  result.standardError.contains(expectation.sourceMarker);

    if (!expectation.thirdRequiredMarker.empty())
    {
      passed = passed && result.standardError.contains(expectation.thirdRequiredMarker);
    }

    if (expectation.scenario == "truncated-diagnostic")
    {
      passed = passed && result.standardError.ends_with('\n');
    }

    if (!expectation.forbiddenMarker.empty())
    {
      passed = passed && !result.standardError.contains(expectation.forbiddenMarker);
    }

    if (expectation.emergencyBeforeSink)
    {
      passed = passed && result.standardError.find("AOBUS_FATAL category=fatal") <
                           result.standardError.find("AOBUS_TEST sink=accepted");
    }

    return passed;
  }
} // namespace

int main(int argc, char* argv[])
{
  if (argc == 3 && std::string_view{argv[1]} == "--aobus-fatal-probe-child")
  {
    return ao::test::runFatalProbeScenario(argv[2]);
  }

  if (argc != 1)
  {
    std::println(stderr, "Usage: ao_fatal_probe [--aobus-fatal-probe-child <scenario>]");
    return 2;
  }

  auto const executablePath = ao::test::fatalProbeExecutablePath();

  if (executablePath.empty())
  {
    std::println(stderr, "ao_fatal_probe could not resolve its executable path");
    return 1;
  }

  for (auto const& expectation : ao::test::fatalProbeExpectations())
  {
    auto const result = ao::test::runFatalProbe(executablePath, expectation.scenario, kProbeTimeout);

    if (!verifyProbe(expectation, result))
    {
      std::println(stderr,
                   "ao_fatal_probe scenario '{}' failed: started={} timed-out={} exited={} "
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
