// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/AudioFatalProbeProtocol.h"
#include "test/fatal/AudioFatalProbeScenario.h"
#include "test/fatal/FatalProbeProcess.h"

#include <chrono>
#include <print>
#include <string_view>

namespace
{
  constexpr auto kProbeTimeout = std::chrono::seconds{15};

  bool verifyProbe(ao::audio::test::AudioFatalProbeExpectation const& expectation,
                   ao::test::FatalProbeResult const& result)
  {
    return result.started && result.launchError.empty() && !result.timedOut && result.endedByPlatformAbort() &&
           result.standardError.contains("AOBUS_FATAL") && result.standardError.contains(expectation.category) &&
           result.standardError.contains(expectation.context) && result.standardError.contains("source=");
  }
} // namespace

int main(int argc, char* argv[])
{
  if (argc == 3 && std::string_view{argv[1]} == "--aobus-fatal-probe-child")
  {
    return ao::audio::test::runAudioFatalProbeScenario(argv[2]);
  }

  if (argc != 1)
  {
    std::println(stderr, "Usage: ao_audio_fatal_probe [--aobus-fatal-probe-child <scenario>]");
    return 2;
  }

  auto const executablePath = ao::test::currentProbeExecutablePath();

  if (executablePath.empty())
  {
    std::println(stderr, "ao_audio_fatal_probe could not resolve its executable path");
    return 1;
  }

  for (auto const& expectation : ao::audio::test::audioFatalProbeExpectations())
  {
    auto const result = ao::test::runFatalProbe(executablePath, expectation.scenario, kProbeTimeout);

    if (!verifyProbe(expectation, result))
    {
      std::println(stderr,
                   "ao_audio_fatal_probe scenario '{}' failed: started={} timed-out={} exited={} "
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
