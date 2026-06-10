// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/fleet/Model.h>
#include <ao/fleet/ProcessRunner.h>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/asio/write.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/process/v2/stdio.hpp>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <memory>
#include <stop_token>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <utility>
#include <vector>

namespace ao::fleet
{
  namespace
  {
    namespace asio = boost::asio;
    namespace process = boost::process::v2;

    constexpr auto kWatchdogPollInterval = std::chrono::milliseconds{20};
    constexpr auto kShellSignalExitBase = 128;
    constexpr auto kShellSignalExitMax = 192;

    asio::awaitable<void> drain(asio::readable_pipe pipe, std::shared_ptr<std::string> outputPtr)
    {
      auto buffer = std::array<char, 8192>{};

      while (true)
      {
        auto [error, count] = co_await pipe.async_read_some(asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));

        if (count > 0)
        {
          outputPtr->append(buffer.data(), count);
        }

        if (error)
        {
          break;
        }
      }
    }

    asio::awaitable<void> writeInput(asio::writable_pipe pipe, std::string input)
    {
      if (!input.empty())
      {
        co_await asio::async_write(pipe, asio::buffer(input), asio::use_awaitable);
      }

      pipe.close();
    }

    std::vector<std::string> buildEnvironment(ProcessRequest const& request)
    {
      auto result = std::vector<std::string>{};

      for (auto const& name : request.environmentWhitelist)
      {
        if (auto const* value = std::getenv(name.c_str()); value != nullptr)
        {
          result.push_back(name + "=" + value);
        }
      }

      for (auto const& [name, value] : request.environment)
      {
        auto const prefix = name + "=";
        std::erase_if(result, [&](std::string const& row) { return row.starts_with(prefix); });
        result.push_back(prefix + value);
      }

      return result;
    }

    asio::awaitable<void> waitForChildExit(std::shared_ptr<process::process> childPtr,
                                           std::shared_ptr<std::atomic_bool> completedPtr)
    {
      [[maybe_unused]] auto const _ = co_await childPtr->async_wait(asio::use_awaitable);
      completedPtr->store(true, std::memory_order_release);
    }
  } // namespace

  ProcessResult BoostProcessRunner::run(ProcessRequest const& request)
  {
    auto result = ProcessResult{};
    auto const started = std::chrono::steady_clock::now();

    if (request.argv.empty())
    {
      result.standardError = "empty argv";
      return result;
    }

    try
    {
      auto context = asio::io_context{};
      auto command = request.argv.front().find('/') == std::string::npos
                       ? process::environment::find_executable(request.argv.front())
                       : boost::filesystem::path{request.argv.front()};
      auto executable = process::environment::find_executable("setsid");

      if (command.empty())
      {
        result.standardError = "executable not found: " + request.argv.front();
        return result;
      }

      if (executable.empty())
      {
        result.standardError = "setsid executable not found";
        return result;
      }

      auto arguments = std::vector<std::string>{"--wait", command.string()};
      arguments.insert(arguments.end(), request.argv.begin() + 1, request.argv.end());
      auto outputPipe = asio::readable_pipe{context};
      auto errorPipe = asio::readable_pipe{context};
      auto inputPipe = asio::writable_pipe{context};
      auto environment = buildEnvironment(request);
      auto childPtr = std::make_shared<process::process>(
        context,
        executable,
        arguments,
        process::process_environment{environment},
        process::process_start_dir{boost::filesystem::path{request.cwd}},
        process::process_stdio{.in = inputPipe, .out = outputPipe, .err = errorPipe});

      auto const pid = static_cast<pid_t>(childPtr->id());
      auto completedPtr = std::make_shared<std::atomic_bool>(false);
      auto timedOutPtr = std::make_shared<std::atomic_bool>(false);
      auto stdoutDataPtr = std::make_shared<std::string>();
      auto stderrDataPtr = std::make_shared<std::string>();

      auto watchdog = std::jthread{
        [pid, completedPtr, timedOutPtr, timeout = request.timeout, grace = request.terminationGrace](
          std::stop_token stop)
        {
          auto sleepUntilStopped = [&stop](std::chrono::milliseconds duration)
          {
            auto const end = std::chrono::steady_clock::now() + duration;

            while (!stop.stop_requested() && std::chrono::steady_clock::now() < end)
            {
              std::this_thread::sleep_for(std::min(
                kWatchdogPollInterval,
                std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now())));
            }
          };

          sleepUntilStopped(timeout);

          if (stop.stop_requested() || completedPtr->load(std::memory_order_acquire))
          {
            return;
          }

          timedOutPtr->store(true, std::memory_order_release);
          [[maybe_unused]] auto const _ = ::kill(-pid, SIGTERM);
          sleepUntilStopped(grace);

          if (!completedPtr->load(std::memory_order_acquire))
          {
            [[maybe_unused]] auto const _ = ::kill(-pid, SIGKILL);
          }
        }};

      asio::co_spawn(context, drain(std::move(outputPipe), stdoutDataPtr), asio::detached);
      asio::co_spawn(context, drain(std::move(errorPipe), stderrDataPtr), asio::detached);
      asio::co_spawn(context, writeInput(std::move(inputPipe), request.standardInput), asio::detached);
      asio::co_spawn(context, waitForChildExit(childPtr, completedPtr), asio::detached);
      context.run();
      watchdog.request_stop();

      result.standardOutput = std::move(*stdoutDataPtr);
      result.standardError = std::move(*stderrDataPtr);
      result.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
      auto const nativeStatus = childPtr->native_exit_code();
      result.exitCode = childPtr->exit_code();

      if (timedOutPtr->load(std::memory_order_acquire))
      {
        result.status = ProcessStatus::TimedOut;
      }
      else if (WIFSIGNALED(nativeStatus))
      {
        result.status = ProcessStatus::Signaled;
        result.signal = WTERMSIG(nativeStatus);
      }
      else if (result.exitCode >= kShellSignalExitBase && result.exitCode <= kShellSignalExitMax)
      {
        result.status = ProcessStatus::Signaled;
        result.signal = result.exitCode - kShellSignalExitBase;
      }
      else
      {
        result.status = ProcessStatus::Exited;
      }

      return result;
    }
    catch (std::exception const& exception)
    {
      result.standardError = exception.what();
      result.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
      return result;
    }
  }
} // namespace ao::fleet
