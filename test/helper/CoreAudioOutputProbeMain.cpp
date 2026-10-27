// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/audio/backend/CoreAudioTestSupport.h"
#include <ao/Error.h>
#include <ao/audio/Device.h>

#include <stdlib.h> // NOLINT(modernize-deprecated-headers) -- POSIX setenv/unsetenv require this header.

#include <cstdio>
#include <print>
#include <string_view>
#include <utility>

namespace
{
  constexpr auto kDeviceUidEnvironment = "AOBUS_TEST_COREAUDIO_DEVICE_UID";
  // Deliberately unknown fixture UID: the missing scenario resolves a real
  // enumeration against a value that is not expected to match any device.
  constexpr auto kMissingDeviceUid = "ao-coreaudio-probe-deliberately-missing-device-uid";

  bool tryApplyScenario(std::string_view const scenario)
  {
    // The probe mutates only this child's own environment, before any
    // application code observes it, so the parent test process stays unchanged.
    // Darwin's public stdlib.h reexports these POSIX declarations from _stdlib.h.
    if (scenario == "unset")
    {
      // NOLINTNEXTLINE(misc-include-cleaner) -- Darwin stdlib.h reexport described above.
      return ::unsetenv(kDeviceUidEnvironment) == 0;
    }

    if (scenario == "empty")
    {
      // NOLINTNEXTLINE(misc-include-cleaner) -- Darwin stdlib.h reexport described above.
      return ::setenv(kDeviceUidEnvironment, "", 1) == 0;
    }

    if (scenario == "missing")
    {
      // NOLINTNEXTLINE(misc-include-cleaner) -- Darwin stdlib.h reexport described above.
      return ::setenv(kDeviceUidEnvironment, kMissingDeviceUid, 1) == 0;
    }

    return false;
  }

  void reportRequestedOutput(std::string_view const scenario)
  {
    auto const requestedRes = ao::audio::test::requestedCoreAudioTestOutput();

    if (!requestedRes)
    {
      std::print("{}: error code={}\n", scenario, std::to_underlying(requestedRes.error().code));
      return;
    }

    if (auto const& optDevice = *requestedRes; optDevice)
    {
      std::print("{}: selected {}\n", scenario, optDevice->id.raw());
      return;
    }

    std::print("{}: absent\n", scenario);
  }
} // namespace

int main(int argc, char* argv[])
{
  if (argc == 3 && std::string_view{argv[1]} == "--aobus-probe-child")
  {
    auto const scenario = std::string_view{argv[2]};

    if (!tryApplyScenario(scenario))
    {
      std::println(stderr, "ao_coreaudio_output_probe: unknown scenario or rejected environment '{}'", scenario);
      return 2;
    }

    reportRequestedOutput(scenario);
    return 0;
  }

  std::println(stderr, "Usage: ao_coreaudio_output_probe [--aobus-probe-child <scenario>]");
  return 2;
}
