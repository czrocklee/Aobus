// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "council/ProcessRunner.h"

#include "council/Model.h"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/asio/write.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/posix/default_launcher.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/system/error_code.hpp>
#include <unistd.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::council
{
  namespace
  {
    namespace asio = boost::asio;
    namespace process = boost::process::v2;

    constexpr auto kShellSignalExitBase = 128;
    constexpr auto kShellSignalExitMax = 192;
    // Upper bound for reaping a SIGKILLed child; SIGKILL cannot be caught, so this never fires in practice.
    constexpr auto kReapInterval = std::chrono::hours{1};
    constexpr auto kStandardPipeCount = 3;
    constexpr auto kTruncationMarker = std::string_view{"\n[... output truncated by aobus-council ...]\n"};

    // The runner writes to pipes whose read end may close at any time (a child is free to exit
    // without reading stdin), so EPIPE must surface as an error code instead of a fatal SIGPIPE.
    // Children spawned by the launcher get SIG_DFL back via ResetSigpipeInChild below.
    void ignoreSigpipeOnce()
    {
      [[maybe_unused]] static auto const installed = std::signal(SIGPIPE, SIG_IGN);
    }

    struct ResetSigpipeInChild final
    {
      static boost::system::error_code on_exec_setup(process::posix::default_launcher& /*launcher*/,
                                                     boost::process::v2::filesystem::path const& /*executable*/,
                                                     char const* const*(& /*commandLine*/))
      {
        // SIG_IGN survives execve, so without this the spawned tree would inherit the parent's
        // SIGPIPE suppression and shell pipelines inside child commands would change behavior.
        std::signal(SIGPIPE, SIG_DFL);
        return {};
      }
    };

    // Opens a live sink for incremental output flushing. The file is always written through
    // an append-mode stream (O_APPEND) so stdout and stderr sinks naming the same file
    // interleave at chunk granularity instead of clobbering each other's offsets; a
    // non-append sink is truncated once up front. Sink failures are diagnostics: the
    // in-memory capture stays authoritative, so a failed open degrades to the old behavior.
    std::ofstream openStreamSink(std::optional<StreamSink> const& optSink, std::string& warnings)
    {
      auto result = std::ofstream{};

      if (!optSink)
      {
        return result;
      }

      auto directoryError = std::error_code{};

      if (!optSink->path.parent_path().empty())
      {
        std::filesystem::create_directories(optSink->path.parent_path(), directoryError);
      }

      if (!directoryError && !optSink->append)
      {
        std::ofstream{optSink->path, std::ios::binary | std::ios::trunc};
      }

      result.open(optSink->path, std::ios::binary | std::ios::app);

      if (!result.is_open())
      {
        warnings += "process runner: cannot open sink " + optSink->path.string() + "\n";
      }

      return result;
    }

    // Everything below runs on the single thread driving io_context::run(), so plain members
    // are safe without synchronization.
    struct RunState final
    {
      explicit RunState(asio::io_context& context)
        : timer{context}, outputPipe{context}, errorPipe{context}, inputPipe{context}
      {
      }

      asio::steady_timer timer;
      asio::readable_pipe outputPipe;
      asio::readable_pipe errorPipe;
      asio::writable_pipe inputPipe;
      std::string standardOutput;
      std::string standardError;
      std::ofstream outputSinkFile;
      std::ofstream errorSinkFile;
      std::size_t outputBytes = 0;
      std::size_t errorBytes = 0;
      std::size_t openPipes = kStandardPipeCount;
      bool childExited = false;
      bool timedOut = false;
      bool outputTruncated = false;
      bool errorTruncated = false;

      void notePipeClosed()
      {
        if (--openPipes == 0)
        {
          timer.cancel();
        }
      }

      void forceClosePipes()
      {
        auto code = boost::system::error_code{};
        std::ignore = outputPipe.close(code);
        std::ignore = errorPipe.close(code);
        std::ignore = inputPipe.close(code);
      }
    };

    // Reduce `length` so that text[0, length) does not end in the middle of a
    // multibyte UTF-8 sequence. Applied only at the truncation seam, where the
    // dropped tail is discarded anyway, so the captured text stays well-formed.
    std::size_t utf8SafeTruncation(std::string_view text, std::size_t length)
    {
      if (length == 0 || length > text.size())
      {
        return length;
      }

      constexpr std::uint32_t kUtf8ContinuationMask = 0xC0U;
      constexpr std::uint32_t kUtf8ContinuationValue = 0x80U;

      // Walk back to the lead byte of the code point that occupies the seam.
      std::size_t start = length - 1;

      while (start > 0 && (static_cast<std::uint32_t>(static_cast<unsigned char>(text[start])) &
                           kUtf8ContinuationMask) == kUtf8ContinuationValue)
      {
        --start;
      }

      constexpr std::uint32_t kUtf8AsciiLimit = 0x80U;
      constexpr std::uint32_t kUtf8TwoByteShift = 5U;
      constexpr std::uint32_t kUtf8TwoByteValue = 0x6U;
      constexpr std::uint32_t kUtf8ThreeByteShift = 4U;
      constexpr std::uint32_t kUtf8ThreeByteValue = 0xEU;
      constexpr std::uint32_t kUtf8FourByteShift = 3U;
      constexpr std::uint32_t kUtf8FourByteValue = 0x1EU;

      std::uint32_t const lead = static_cast<std::uint32_t>(static_cast<unsigned char>(text[start]));
      std::size_t expected = 1;

      if (lead < kUtf8AsciiLimit)
      {
        expected = 1;
      }
      else if ((lead >> kUtf8TwoByteShift) == kUtf8TwoByteValue)
      {
        expected = 2;
      }
      else if ((lead >> kUtf8ThreeByteShift) == kUtf8ThreeByteValue)
      {
        expected = 3;
      }
      else if ((lead >> kUtf8FourByteShift) == kUtf8FourByteValue)
      {
        expected = 4;
      }

      // Keep the whole prefix when the trailing sequence is complete; otherwise
      // drop the partial lead so no split multibyte character survives.
      return start + expected <= length ? length : start;
    }

    void appendBounded(std::string& memory,
                       std::ofstream& file,
                       std::size_t& byteCount,
                       bool& truncated,
                       std::string_view chunk,
                       std::size_t limit)
    {
      auto const remaining = byteCount < limit ? limit - byteCount : std::size_t{};
      auto const truncating = chunk.size() > remaining;
      auto const accepted = truncating ? utf8SafeTruncation(chunk, remaining) : chunk.size();

      if (accepted > 0)
      {
        memory.append(chunk.substr(0, accepted));

        if (file.is_open())
        {
          file << chunk.substr(0, accepted);
          file.flush();
        }
      }

      if (!truncating)
      {
        byteCount += accepted;
        return;
      }

      // Saturate the budget so later chunks cannot land after the marker.
      byteCount = limit;

      if (!truncated)
      {
        memory.append(kTruncationMarker);

        if (file.is_open())
        {
          file.write(kTruncationMarker.data(), static_cast<std::streamsize>(kTruncationMarker.size()));
          file.flush();
        }

        truncated = true;
      }
    }

    asio::awaitable<void> drain(std::shared_ptr<RunState> statePtr, bool isError, std::size_t limit)
    {
      constexpr std::size_t kReadBufferSize = 8192;

      auto& pipe = isError ? statePtr->errorPipe : statePtr->outputPipe;
      auto& sink = isError ? statePtr->standardError : statePtr->standardOutput;
      auto& sinkFile = isError ? statePtr->errorSinkFile : statePtr->outputSinkFile;
      auto& byteCount = isError ? statePtr->errorBytes : statePtr->outputBytes;
      auto& truncated = isError ? statePtr->errorTruncated : statePtr->outputTruncated;
      auto buffer = std::array<char, kReadBufferSize>{};

      while (true)
      {
        auto [error, count] = co_await pipe.async_read_some(asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));

        if (count > 0)
        {
          appendBounded(sink, sinkFile, byteCount, truncated, std::string_view{buffer.data(), count}, limit);
        }

        if (error)
        {
          break;
        }
      }

      statePtr->notePipeClosed();
    }

    asio::awaitable<void> writeInput(std::shared_ptr<RunState> statePtr, std::string input)
    {
      if (!input.empty())
      {
        // A broken pipe is legal here: the child may exit (or close stdin) without reading.
        [[maybe_unused]] auto const result =
          co_await asio::async_write(statePtr->inputPipe, asio::buffer(input), asio::as_tuple(asio::use_awaitable));
      }

      auto code = boost::system::error_code{};
      std::ignore = statePtr->inputPipe.close(code);
      statePtr->notePipeClosed();
    }

    asio::awaitable<void> waitForChildExit(std::shared_ptr<process::process> childPtr,
                                           std::shared_ptr<RunState> statePtr)
    {
      [[maybe_unused]] auto const result = co_await childPtr->async_wait(asio::as_tuple(asio::use_awaitable));
      statePtr->childExited = true;
      statePtr->timer.cancel();
    }

    // Owns the shared timer: enforces the run timeout (TERM, then KILL after the grace period)
    // and, once the child has exited, bounds how long orphaned grandchildren may keep the
    // stdio pipes open before the whole process group is killed and the pipes force-closed.
    // Without that last step a backgrounded grandchild inheriting stderr would keep the drain
    // coroutines pending and io_context::run() would never return.
    asio::awaitable<void> supervise(std::shared_ptr<RunState> statePtr,
                                    pid_t pid,
                                    std::chrono::milliseconds timeout,
                                    std::chrono::milliseconds gracePeriod)
    {
      auto deadline = std::chrono::steady_clock::now() + timeout;
      bool killed = false;

      while (!statePtr->childExited)
      {
        statePtr->timer.expires_at(deadline);
        auto [error] = co_await statePtr->timer.async_wait(asio::as_tuple(asio::use_awaitable));

        if (statePtr->childExited)
        {
          break;
        }

        if (error)
        {
          // Cancelled because all pipes closed while the child still runs; keep waiting.
          continue;
        }

        if (!statePtr->timedOut)
        {
          statePtr->timedOut = true;
          [[maybe_unused]] auto const termResult = ::kill(-pid, SIGTERM);
          deadline = std::chrono::steady_clock::now() + gracePeriod;
        }
        else if (!killed)
        {
          killed = true;
          [[maybe_unused]] auto const killResult = ::kill(-pid, SIGKILL);
          deadline = std::chrono::steady_clock::now() + kReapInterval;
        }
        else
        {
          break;
        }
      }

      if (statePtr->openPipes > 0)
      {
        statePtr->timer.expires_after(gracePeriod);
        auto [error] = co_await statePtr->timer.async_wait(asio::as_tuple(asio::use_awaitable));

        if (!error)
        {
          [[maybe_unused]] auto const killResult = ::kill(-pid, SIGKILL);
          statePtr->forceClosePipes();
        }
      }
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
  } // namespace

  ProcessResult BoostProcessRunner::run(ProcessRequest const& request)
  {
    auto result = ProcessResult{};
    auto const start = std::chrono::steady_clock::now();

    if (request.argv.empty())
    {
      result.standardError = "empty argv";
      return result;
    }

    ignoreSigpipeOnce();

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
      auto statePtr = std::make_shared<RunState>(context);
      // Sink-open warnings land at the front of the captured stderr; the drain coroutines
      // only ever append, so the child's own stderr follows them.
      statePtr->outputSinkFile = openStreamSink(request.optStdoutSink, statePtr->standardError);
      statePtr->errorSinkFile = openStreamSink(request.optStderrSink, statePtr->standardError);
      auto environment = buildEnvironment(request);
      auto childPtr = std::make_shared<process::process>(
        context,
        executable,
        arguments,
        process::process_environment{environment},
        process::process_start_dir{boost::filesystem::path{request.cwd}},
        process::process_stdio{.in = statePtr->inputPipe, .out = statePtr->outputPipe, .err = statePtr->errorPipe},
        ResetSigpipeInChild{});

      auto const pid = static_cast<pid_t>(childPtr->id());

      // The child is already running: a throwing launch callback must not abort before the
      // supervision and drain coroutines are installed, or the process would run unsupervised.
      if (request.onLaunch)
      {
        try
        {
          request.onLaunch(pid);
        }
        catch (std::exception const& exception)
        {
          statePtr->standardError +=
            std::string{"process runner: onLaunch callback failed: "} + exception.what() + "\n";
        }
      }

      asio::co_spawn(context, drain(statePtr, false, request.maxCapturedBytes), asio::detached);
      asio::co_spawn(context, drain(statePtr, true, request.maxCapturedBytes), asio::detached);
      asio::co_spawn(context, writeInput(statePtr, request.standardInput), asio::detached);
      asio::co_spawn(context, waitForChildExit(childPtr, statePtr), asio::detached);
      asio::co_spawn(
        context, supervise(statePtr, pid, request.timeout, request.terminationGracePeriod), asio::detached);
      context.run();

      result.standardOutput = std::move(statePtr->standardOutput);
      result.standardError = std::move(statePtr->standardError);
      result.standardOutputTruncated = statePtr->outputTruncated;
      result.standardErrorTruncated = statePtr->errorTruncated;
      result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
      auto const nativeStatus = childPtr->native_exit_code();
      result.exitCode = childPtr->exit_code();

      if (statePtr->timedOut)
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
      result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
      return result;
    }
  }
} // namespace ao::council
