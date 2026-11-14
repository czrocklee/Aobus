// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <csignal>
#endif

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

  /**
   * Runs `executablePath --aobus-probe-child <scenario>` and captures its output.
   *
   * On POSIX hosts `timeout` starts once the child calls
   * reportProbeChildStarted() or exits. Host work before the child's first
   * instruction is not scenario time: macOS can hold a new ad-hoc signed
   * process there while its execution policy checks the binary, which takes
   * longer than a scenario timeout when parallel suites launch many probes. A
   * child that never starts still times out, after a fixed startup limit.
   * Windows measures from process creation.
   */
  ProbeProcessResult runProbeProcess(std::filesystem::path const& executablePath,
                                     std::string_view scenario,
                                     std::chrono::milliseconds timeout);

  /**
   * Tells the runProbeProcess() parent that this probe child is running.
   *
   * Every `--aobus-probe-child` entry calls this first. It does nothing outside
   * a probe child and on Windows.
   */
  void reportProbeChildStarted() noexcept;

  /**
   * Keeps a probe child's intentional abort out of host crash collection.
   *
   * A probe child that can abort calls this before running its scenario.
   * Linux hands a crash to a pipe handler such as systemd-coredump even when
   * RLIMIT_CORE is zero, so the child makes itself non-dumpable. Windows aborts
   * already bypass error reporting in the fatal backend, and macOS crash
   * reporting is unchanged.
   */
  void suppressProbeChildCrashDump() noexcept;
} // namespace ao::test
