// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/TuiSignalProbeScenario.h"

#include "tui/SignalExitWatcher.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <print>
#include <string_view>
#ifdef _WIN32
#include <cstdio>
#else
#include <thread>
#endif

#ifndef _WIN32
#include <signal.h> // NOLINT(modernize-deprecated-headers) -- POSIX signal APIs require this header.
#undef sigemptyset
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ao::tui::test
{
  namespace
  {
#ifndef _WIN32
    std::atomic_int gPreviousHandlerCount{0}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

    void previousSigintHandler(int /*signal*/) // NOLINT(aobus-modernize-use-std-numbers)
    {
      gPreviousHandlerCount.fetch_add(1);
    }
#endif

    bool waitForFlag(std::atomic_bool& flag, std::condition_variable& cv, std::mutex& mutex)
    {
      auto lock = std::unique_lock{mutex};
      return cv.wait_for(lock, std::chrono::seconds{2}, [&] { return flag.load(); });
    }

#ifndef _WIN32
    using SignalAction = struct sigaction;

    std::int32_t routeSignal(int const signal)
    {
      auto mutex = std::mutex{};
      auto cv = std::condition_variable{};
      auto called = std::atomic_bool{false};
      auto watcher = SignalExitWatcher{[&]
                                       {
                                         called.store(true);
                                         cv.notify_all();
                                       }};

      if (::raise(signal) != 0)
      {
        std::println(stderr, "raise failed");
        return 1;
      }

      if (!waitForFlag(called, cv, mutex))
      {
        std::println(stderr, "callback was not invoked");
        return 1;
      }

      std::println("routed");
      return 0;
    }

    std::int32_t restorePreviousHandler()
    {
      gPreviousHandlerCount.store(0);

      auto previous = SignalAction{};
      previous.sa_handler = previousSigintHandler;
      previous.sa_flags = 0;
      ::sigemptyset(&previous.sa_mask);

      if (::sigaction(SIGINT, &previous, nullptr) != 0)
      {
        std::println(stderr, "failed to install previous handler");
        return 1;
      }

      {
        auto watcher = SignalExitWatcher{[] {}};
      }

      if (::raise(SIGINT) != 0)
      {
        std::println(stderr, "raise failed");
        return 1;
      }

      auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};

      while (gPreviousHandlerCount.load() == 0 && std::chrono::steady_clock::now() < deadline)
      {
        std::this_thread::yield();
      }

      if (gPreviousHandlerCount.load() == 0)
      {
        std::println(stderr, "previous handler was not restored");
        return 1;
      }

      std::println("restored");
      return 0;
    }
#endif

#ifdef _WIN32
    std::int32_t routeCtrlC()
    {
      if (::GetConsoleWindow() == nullptr)
      {
        [[maybe_unused]] auto const allocated = ::AllocConsole();
      }

      auto mutex = std::mutex{};
      auto cv = std::condition_variable{};
      auto called = std::atomic_bool{false};
      auto watcher = SignalExitWatcher{[&]
                                       {
                                         called.store(true);
                                         cv.notify_all();
                                       }};

      if (::GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) == 0 || !waitForFlag(called, cv, mutex))
      {
        // Session-0 SSH and redirected-stdio children often have no console
        // that can deliver CTRL_C_EVENT. Keep the scenario invokable from an
        // interactive console; do not treat that host gap as a passing probe.
        std::println(stderr, "CTRL_C_EVENT was not delivered");
        return kTuiSignalProbeSkipped;
      }

      std::println("routed");
      return 0;
    }
#endif
  } // namespace

  std::int32_t runTuiSignalProbeScenario(char const* const scenario)
  {
    auto const name = std::string_view{scenario == nullptr ? "" : scenario};

#ifndef _WIN32

    if (name == "sigint")
    {
      return routeSignal(SIGINT);
    }

    if (name == "sigterm")
    {
      return routeSignal(SIGTERM);
    }

    if (name == "sighup")
    {
      return routeSignal(SIGHUP);
    }

    if (name == "restore-sigint")
    {
      return restorePreviousHandler();
    }

#else

    if (name == "ctrl-c")
    {
      return routeCtrlC();
    }

#endif

    std::println(stderr, "unknown TUI signal probe scenario '{}'", name);
    return 2;
  }
} // namespace ao::tui::test
