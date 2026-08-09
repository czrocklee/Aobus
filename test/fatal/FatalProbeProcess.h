// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace ao::test
{
  struct FatalProbeResult final
  {
    bool started = false;
    bool timedOut = false;
    bool exited = false;
    bool signaled = false;
    std::uint32_t exitCode = 0;
    std::int32_t signalNumber = 0;
    std::string standardError;
    std::string launchError;

    bool endedByFatalTermination() const noexcept
    {
      return started && !timedOut && (signaled || (exited && exitCode != 0));
    }

    bool endedByPlatformAbort() const noexcept
    {
#ifdef _WIN32
      return started && !timedOut && exited && exitCode != 0;
#else
      return started && !timedOut && signaled && signalNumber == SIGABRT;
#endif
    }
  };

  std::filesystem::path currentProbeExecutablePath();
  std::filesystem::path siblingProbeExecutablePath(std::string_view executableStem);
  std::filesystem::path fatalProbeExecutablePath();

  FatalProbeResult runFatalProbe(std::filesystem::path const& executablePath,
                                 std::string_view scenario,
                                 std::chrono::milliseconds timeout);
} // namespace ao::test
