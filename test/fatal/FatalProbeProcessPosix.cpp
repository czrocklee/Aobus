// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/FatalProbeProcess.h"

#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <system_error>
#include <thread>

namespace ao::test
{
  namespace
  {
    constexpr std::size_t kMaximumCapturedBytes = std::size_t{64} * 1024;

    void closeFileDescriptor(int descriptor) noexcept
    {
      if (descriptor >= 0)
      {
        [[maybe_unused]] auto const result = ::close(descriptor);
      }
    }

    void captureStandardError(int descriptor, std::string& output)
    {
      auto buffer = std::array<char, 4096>{};

      for (;;)
      {
        auto const readSize = ::read(descriptor, buffer.data(), buffer.size());

        if (readSize > 0)
        {
          auto const available = kMaximumCapturedBytes - output.size();
          auto const capturedSize = std::min(static_cast<std::size_t>(readSize), available);
          output.append(buffer.data(), capturedSize);
          continue;
        }

        if (readSize < 0 && errno == EINTR)
        {
          continue;
        }

        break;
      }
    }

    pid_t waitForProcess(pid_t const processId, int& waitStatus) noexcept
    {
      for (;;)
      {
        pid_t const reapedProcess = ::waitpid(processId, &waitStatus, 0);

        if (reapedProcess >= 0 || errno != EINTR)
        {
          return reapedProcess;
        }
      }
    }
  } // namespace

  std::filesystem::path currentProbeExecutablePath()
  {
    auto error = std::error_code{};
    auto executablePath = std::filesystem::read_symlink("/proc/self/exe", error);

    if (error)
    {
      return {};
    }

    return executablePath;
  }

  std::filesystem::path fatalProbeExecutablePath()
  {
    return siblingProbeExecutablePath("ao_fatal_probe");
  }

  std::filesystem::path siblingProbeExecutablePath(std::string_view const executableStem)
  {
    auto executablePath = currentProbeExecutablePath();

    if (!executablePath.empty())
    {
      executablePath.replace_filename(executableStem);
    }

    return executablePath;
  }

  FatalProbeResult runFatalProbe(std::filesystem::path const& executablePath,
                                 std::string_view scenarioName,
                                 std::chrono::milliseconds timeout)
  {
    auto result = FatalProbeResult{};
    auto pipeDescriptors = std::array<int, 2>{-1, -1};

    if (::pipe(pipeDescriptors.data()) != 0)
    {
      result.launchError = std::strerror(errno);
      return result;
    }

    auto actions = posix_spawn_file_actions_t{};
    auto const initializationStatus = ::posix_spawn_file_actions_init(&actions);
    auto actionStatus = initializationStatus;

    if (actionStatus == 0)
    {
      actionStatus = ::posix_spawn_file_actions_adddup2(&actions, pipeDescriptors[1], STDERR_FILENO);
    }

    if (actionStatus == 0)
    {
      actionStatus = ::posix_spawn_file_actions_addclose(&actions, pipeDescriptors[0]);
    }

    if (actionStatus == 0)
    {
      actionStatus = ::posix_spawn_file_actions_addclose(&actions, pipeDescriptors[1]);
    }

    if (actionStatus == 0)
    {
      actionStatus = ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }

    if (actionStatus != 0)
    {
      if (initializationStatus == 0)
      {
        [[maybe_unused]] auto const destroyResult = ::posix_spawn_file_actions_destroy(&actions);
      }

      closeFileDescriptor(pipeDescriptors[0]);
      closeFileDescriptor(pipeDescriptors[1]);
      result.launchError = std::strerror(actionStatus);
      return result;
    }

    auto executable = executablePath.string();
    auto probeOption = std::string{"--aobus-fatal-probe-child"};
    auto scenario = std::string{scenarioName};
    auto arguments = std::array<char*, 4>{executable.data(), probeOption.data(), scenario.data(), nullptr};
    pid_t processId = 0;
    auto const spawnStatus =
      ::posix_spawn(&processId, executable.c_str(), &actions, nullptr, arguments.data(), environ);
    [[maybe_unused]] auto const destroyResult = ::posix_spawn_file_actions_destroy(&actions);
    closeFileDescriptor(pipeDescriptors[1]);
    pipeDescriptors[1] = -1;

    if (spawnStatus != 0)
    {
      closeFileDescriptor(pipeDescriptors[0]);
      result.launchError = std::strerror(spawnStatus);
      return result;
    }

    result.started = true;
    auto standardErrorReader = std::jthread{[descriptor = pipeDescriptors[0], &result]
                                            { captureStandardError(descriptor, result.standardError); }};
    int waitStatus = 0;
    bool childReaped = false;
    auto const deadline = std::chrono::steady_clock::now() + timeout;

    for (;;)
    {
      auto const waitedProcess = ::waitpid(processId, &waitStatus, WNOHANG);

      if (waitedProcess == processId)
      {
        childReaped = true;
        break;
      }

      if (waitedProcess < 0 && errno != EINTR)
      {
        result.launchError = std::strerror(errno);
        [[maybe_unused]] auto const killResult = ::kill(processId, SIGKILL);

        pid_t const reapedProcess = waitForProcess(processId, waitStatus);
        childReaped = reapedProcess == processId;
        break;
      }

      if (std::chrono::steady_clock::now() >= deadline)
      {
        result.timedOut = true;
        [[maybe_unused]] auto const killResult = ::kill(processId, SIGKILL);

        pid_t const reapedProcess = waitForProcess(processId, waitStatus);
        childReaped = reapedProcess == processId;
        break;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    standardErrorReader.join();
    closeFileDescriptor(pipeDescriptors[0]);

    if (childReaped && WIFEXITED(waitStatus))
    {
      result.exited = true;
      result.exitCode = static_cast<std::uint32_t>(WEXITSTATUS(waitStatus));
    }
    else if (childReaped && WIFSIGNALED(waitStatus))
    {
      result.signaled = true;
      result.signalNumber = static_cast<std::int32_t>(WTERMSIG(waitStatus));
    }

    return result;
  }
} // namespace ao::test
