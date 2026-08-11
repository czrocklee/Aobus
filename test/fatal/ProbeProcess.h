// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace ao::test
{
  inline constexpr std::size_t kMaximumProbeOutputBytes = std::size_t{64} * 1024;

  struct ProbeProcessResult final
  {
    bool started = false;
    bool timedOut = false;
    bool exited = false;
    bool signaled = false;
    std::uint32_t exitCode = 0;
    std::int32_t signalNumber = 0;
    std::string standardOutput;
    std::string standardError;
    std::string launchError;

    bool hasSuccessfulExit() const noexcept
    {
      return started && launchError.empty() && !timedOut && exited && exitCode == 0 && !signaled;
    }

    bool hasFatalTermination() const noexcept
    {
      return started && launchError.empty() && !timedOut && (signaled || (exited && exitCode != 0));
    }

    bool hasPlatformAbort() const noexcept
    {
#ifdef _WIN32
      return hasFatalTermination();
#else
      return started && launchError.empty() && !timedOut && signaled && signalNumber == SIGABRT;
#endif
    }
  };

  std::filesystem::path currentProbeExecutablePath();
  std::filesystem::path siblingProbeExecutablePath(std::string_view executableStem);
  std::filesystem::path fatalProbeExecutablePath();

  ProbeProcessResult runProbeProcess(std::filesystem::path const& executablePath,
                                     std::string_view scenario,
                                     std::chrono::milliseconds timeout);
} // namespace ao::test
