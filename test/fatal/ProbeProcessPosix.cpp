// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/ProbeProcess.h"

#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif

#include <signal.h> // NOLINT(modernize-deprecated-headers) -- POSIX process control owns kill and SIGKILL.
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
    // Darwin only declares `environ` for the main executable, so a library
    // translation unit has to go through _NSGetEnviron() instead.
    char** currentEnvironment() noexcept
    {
#ifdef __APPLE__
      return *::_NSGetEnviron();
#else
      return environ;
#endif
    }

    void closeFileDescriptor(int descriptor) noexcept
    {
      if (descriptor >= 0)
      {
        [[maybe_unused]] auto const result = ::close(descriptor);
      }
    }

    void captureOutput(int descriptor, std::string& output)
    {
      auto buffer = std::array<char, 4096>{};

      for (;;)
      {
        auto const readSize = ::read(descriptor, buffer.data(), buffer.size());

        if (readSize > 0)
        {
          auto const available = kMaximumProbeOutputBytes - output.size();
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
#ifdef __APPLE__
    // Darwin has no procfs. _NSGetExecutablePath reports the path the image was
    // loaded from, which may be relative or run through symlinks, so it is
    // resolved before the caller derives sibling probe paths from it. The first
    // call fails on purpose: it reports the buffer size the second one needs.
    std::uint32_t size = 0;
    [[maybe_unused]] auto const sizeQueryStatus = ::_NSGetExecutablePath(nullptr, &size);

    auto buffer = std::string(size, '\0');

    if (::_NSGetExecutablePath(buffer.data(), &size) != 0)
    {
      return {};
    }

    auto error = std::error_code{};
    auto executablePath = std::filesystem::canonical(buffer.c_str(), error);
#else
    auto error = std::error_code{};
    auto executablePath = std::filesystem::read_symlink("/proc/self/exe", error);
#endif

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

  ProbeProcessResult runProbeProcess(std::filesystem::path const& executablePath,
                                     std::string_view scenarioName,
                                     std::chrono::milliseconds timeout)
  {
    auto result = ProbeProcessResult{};
    auto standardOutputPipe = std::array<int, 2>{-1, -1};
    auto standardErrorPipe = std::array<int, 2>{-1, -1};

    if (::pipe(standardOutputPipe.data()) != 0)
    {
      result.launchError = std::strerror(errno);
      return result;
    }

    if (::pipe(standardErrorPipe.data()) != 0)
    {
      auto const pipeError = errno;
      closeFileDescriptor(standardOutputPipe[0]);
      closeFileDescriptor(standardOutputPipe[1]);
      result.launchError = std::strerror(pipeError);
      return result;
    }

#ifdef __APPLE__
    posix_spawn_file_actions_t actions = nullptr;
#else
    auto actions = posix_spawn_file_actions_t{};
#endif
    auto const initializationStatus = ::posix_spawn_file_actions_init(&actions);
    auto actionStatus = initializationStatus;

    if (actionStatus == 0)
    {
      actionStatus = ::posix_spawn_file_actions_adddup2(&actions, standardOutputPipe[1], STDOUT_FILENO);
    }

    if (actionStatus == 0)
    {
      actionStatus = ::posix_spawn_file_actions_adddup2(&actions, standardErrorPipe[1], STDERR_FILENO);
    }

    if (actionStatus == 0)
    {
      actionStatus = ::posix_spawn_file_actions_addclose(&actions, standardOutputPipe[0]);
    }

    if (actionStatus == 0)
    {
      actionStatus = ::posix_spawn_file_actions_addclose(&actions, standardOutputPipe[1]);
    }

    if (actionStatus == 0)
    {
      actionStatus = ::posix_spawn_file_actions_addclose(&actions, standardErrorPipe[0]);
    }

    if (actionStatus == 0)
    {
      actionStatus = ::posix_spawn_file_actions_addclose(&actions, standardErrorPipe[1]);
    }

    if (actionStatus != 0)
    {
      if (initializationStatus == 0)
      {
        [[maybe_unused]] auto const destroyResult = ::posix_spawn_file_actions_destroy(&actions);
      }

      closeFileDescriptor(standardOutputPipe[0]);
      closeFileDescriptor(standardOutputPipe[1]);
      closeFileDescriptor(standardErrorPipe[0]);
      closeFileDescriptor(standardErrorPipe[1]);
      result.launchError = std::strerror(actionStatus);
      return result;
    }

    auto executable = executablePath.string();
    auto probeOption = std::string{"--aobus-probe-child"};
    auto scenario = std::string{scenarioName};
    auto arguments = std::array<char*, 4>{executable.data(), probeOption.data(), scenario.data(), nullptr};
    pid_t processId = 0;
    auto const spawnStatus =
      ::posix_spawn(&processId, executable.c_str(), &actions, nullptr, arguments.data(), currentEnvironment());
    [[maybe_unused]] auto const destroyResult = ::posix_spawn_file_actions_destroy(&actions);
    closeFileDescriptor(standardOutputPipe[1]);
    standardOutputPipe[1] = -1;
    closeFileDescriptor(standardErrorPipe[1]);
    standardErrorPipe[1] = -1;

    if (spawnStatus != 0)
    {
      closeFileDescriptor(standardOutputPipe[0]);
      closeFileDescriptor(standardErrorPipe[0]);
      result.launchError = std::strerror(spawnStatus);
      return result;
    }

    result.started = true;
    auto standardOutputReader =
      std::jthread{[descriptor = standardOutputPipe[0], &result] { captureOutput(descriptor, result.standardOutput); }};
    auto standardErrorReader =
      std::jthread{[descriptor = standardErrorPipe[0], &result] { captureOutput(descriptor, result.standardError); }};
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

    standardOutputReader.join();
    standardErrorReader.join();
    closeFileDescriptor(standardOutputPipe[0]);
    closeFileDescriptor(standardErrorPipe[0]);

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
