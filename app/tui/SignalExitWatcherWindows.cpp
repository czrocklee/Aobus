// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "SignalExitWatcher.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>

namespace ao::tui
{
  namespace
  {
    class ExitCallbackState final
    {
    public:
      explicit ExitCallbackState(std::move_only_function<void()> onExit)
        : _callbackPtr{std::make_shared<std::move_only_function<void()>>(std::move(onExit))}
      {
      }

      void requestExit() noexcept
      {
        auto const lock = std::scoped_lock{_callbackMutex};
        auto callbackPtr = _callbackPtr;

        try
        {
          if (callbackPtr != nullptr && *callbackPtr)
          {
            (*callbackPtr)();
          }
        }
        // NOLINTNEXTLINE(bugprone-empty-catch): Console-control callbacks cannot propagate exceptions.
        catch (...)
        {
          // Console shutdown callbacks are best effort and may not unwind
          // through the Windows console-control dispatcher.
        }
      }

      void disable() noexcept
      {
        // requestExit holds this recursive gate throughout callback execution.
        // An external destructor waits here; a callback that destroys its own
        // watcher can re-enter and clear the member while the local shared_ptr
        // keeps the currently executing callable alive.
        auto const lock = std::scoped_lock{_callbackMutex};
        _callbackPtr.reset();
      }

    private:
      std::recursive_mutex _callbackMutex;
      std::shared_ptr<std::move_only_function<void()>> _callbackPtr;
    };

    std::atomic<std::shared_ptr<ExitCallbackState>>
      gActiveStatePtr{}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

    // Mirrors the POSIX watcher, which handles SIGTERM/SIGHUP and leaves
    // SIGINT to ftxui. On Windows, Ctrl-C must reach the CRT's own console
    // handler (return FALSE below): the CRT raises SIGINT, which ftxui's
    // std::signal handler turns into a graceful loop exit.
    BOOL WINAPI consoleControlHandler(DWORD const controlType)
    {
      auto const statePtr = gActiveStatePtr.load(std::memory_order_acquire);

      switch (controlType)
      {
        case CTRL_C_EVENT: return FALSE;
        case CTRL_BREAK_EVENT:
          if (statePtr != nullptr)
          {
            statePtr->requestExit();
            return TRUE;
          }

          return FALSE;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
          if (statePtr != nullptr)
          {
            statePtr->requestExit();
            // Returning would let Windows terminate the process immediately,
            // cutting cleanup short. Block instead: ExitProcess ends this
            // thread once shutdown completes, and the OS enforces its own
            // grace period (~5s) if shutdown hangs.
            ::Sleep(INFINITE);
          }

          return FALSE;
        default: return FALSE;
      }
    }
  } // namespace

  class SignalExitWatcher::Impl final
  {
  public:
    explicit Impl(std::move_only_function<void()> onExit)
      : _statePtr{std::make_shared<ExitCallbackState>(std::move(onExit))}
    {
      auto expectedPtr = std::shared_ptr<ExitCallbackState>{};

      if (!gActiveStatePtr.compare_exchange_strong(expectedPtr, _statePtr, std::memory_order_acq_rel))
      {
        return;
      }

      _installed = ::SetConsoleCtrlHandler(consoleControlHandler, TRUE) != FALSE;

      if (!_installed)
      {
        expectedPtr = _statePtr;
        std::ignore = gActiveStatePtr.compare_exchange_strong(
          expectedPtr, std::shared_ptr<ExitCallbackState>{}, std::memory_order_acq_rel);
      }
    }

    ~Impl()
    {
      if (_installed)
      {
        auto expectedPtr = _statePtr;
        std::ignore = gActiveStatePtr.compare_exchange_strong(
          expectedPtr, std::shared_ptr<ExitCallbackState>{}, std::memory_order_acq_rel);
        std::ignore = ::SetConsoleCtrlHandler(consoleControlHandler, FALSE);
      }

      _statePtr->disable();
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void requestExit()
    {
      auto statePtr = _statePtr;
      statePtr->requestExit();
    }

  private:
    std::shared_ptr<ExitCallbackState> _statePtr;
    bool _installed = false;
  };

  SignalExitWatcher::SignalExitWatcher(std::move_only_function<void()> onExit)
    : _implPtr{std::make_unique<Impl>(std::move(onExit))}
  {
  }

  SignalExitWatcher::~SignalExitWatcher() = default;

  void SignalExitWatcher::requestExit()
  {
    _implPtr->requestExit();
  }
} // namespace ao::tui
