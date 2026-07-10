// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "SignalExitWatcher.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <tuple>
#include <utility>

namespace ao::tui
{
  namespace
  {
    std::atomic<std::int32_t> gSignalWriteFd{-1};        // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
    std::atomic<std::uint32_t> gActiveSignalHandlers{0}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

    static_assert(decltype(gSignalWriteFd)::is_always_lock_free);
    static_assert(decltype(gActiveSignalHandlers)::is_always_lock_free);

    // POSIX signal handlers must use the C ABI int parameter type. The active
    // count lets teardown wait for a handler that already captured the pipe fd
    // before the global slot was cleared.
    void exitSignalHandler(int /*signal*/) // NOLINT(aobus-modernize-use-std-numbers)
    {
      auto const savedErrno = errno;
      gActiveSignalHandlers.fetch_add(1);

      if (auto const fd = gSignalWriteFd.load(); fd >= 0)
      {
        auto const token = std::byte{1};
        [[maybe_unused]] auto const ignored = ::write(fd, &token, sizeof(token));
      }

      gActiveSignalHandlers.fetch_sub(1);
      errno = savedErrno;
    }
  } // namespace

  class SignalExitWatcher::Impl final
  {
  private:
    class State final
    {
    public:
      explicit State(std::move_only_function<void()> onExit)
        : _onExit{std::move(onExit)}
      {
        if (::pipe(_pipe.data()) == 0 && !makeWriteEndNonBlocking())
        {
          closePipe();
        }
      }

      ~State() { closePipe(); }

      State(State const&) = delete;
      State& operator=(State const&) = delete;
      State(State&&) = delete;
      State& operator=(State&&) = delete;

      bool hasPipe() const noexcept { return _pipe[0] >= 0 && _pipe[1] >= 0; }
      std::int32_t writeFd() const noexcept { return static_cast<std::int32_t>(_pipe[1]); }

      void requestExit()
      {
        if (hasPipe())
        {
          wake();
        }
        else
        {
          invokeExitCallback();
        }
      }

      void stop()
      {
        _running.store(false);
        wake();
      }

      void run()
      {
        while (_running.load())
        {
          auto token = std::byte{};
          auto const result = ::read(_pipe[0], &token, 1);

          if (result > 0)
          {
            if (_running.load())
            {
              invokeExitCallback();
            }

            continue;
          }

          if (result < 0 && errno == EINTR)
          {
            continue;
          }

          break;
        }
      }

    private:
      void wake() const
      {
        if (_pipe[1] >= 0)
        {
          auto const token = std::byte{1};
          [[maybe_unused]] auto const ignored = ::write(_pipe[1], &token, sizeof(token));
        }
      }

      bool makeWriteEndNonBlocking() const
      {
        if (auto const flags = ::fcntl(_pipe[1], F_GETFL, 0); flags >= 0)
        {
          auto const result =
            ::fcntl(_pipe[1], F_SETFL, flags | O_NONBLOCK); // NOLINT(cppcoreguidelines-pro-type-vararg)
          return result == 0;
        }

        return false;
      }

      void closePipe()
      {
        // Close the handler-facing write end first. Teardown has already
        // cleared the global fd and waited for active handlers.
        closeFd(_pipe[1]);
        closeFd(_pipe[0]);
      }

      static void closeFd(int& fd)
      {
        if (fd >= 0)
        {
          std::ignore = ::close(fd);
          fd = -1;
        }
      }

      void invokeExitCallback() noexcept
      {
        try
        {
          if (_onExit)
          {
            _onExit();
          }
        }
        catch (...)
        {
          // Signal-triggered shutdown is best effort and must not terminate the
          // watcher thread if an exit callback fails.
        }
      }

      std::move_only_function<void()> _onExit;
      std::array<int, 2> _pipe{-1, -1};
      std::atomic_bool _running{true};
    };

  public:
    explicit Impl(std::move_only_function<void()> onExit)
      : _statePtr{std::make_shared<State>(std::move(onExit))}
    {
      if (!_statePtr->hasPipe())
      {
        return;
      }

      _thread = std::thread{[statePtr = _statePtr] { statePtr->run(); }};

      auto expected = std::int32_t{-1};
      _ownsSignalHandlers = gSignalWriteFd.compare_exchange_strong(expected, _statePtr->writeFd());

      if (_ownsSignalHandlers)
      {
        _termInstalled = install(SIGTERM, _oldTerm);
        _hupInstalled = install(SIGHUP, _oldHup);
      }
    }

    ~Impl()
    {
      if (_ownsSignalHandlers)
      {
        restore(SIGTERM, _oldTerm, _termInstalled);
        restore(SIGHUP, _oldHup, _hupInstalled);
        auto expected = _statePtr->writeFd();
        std::ignore = gSignalWriteFd.compare_exchange_strong(expected, -1);

        while (gActiveSignalHandlers.load() != 0)
        {
          std::this_thread::yield();
        }
      }

      _statePtr->stop();

      if (!_thread.joinable())
      {
        return;
      }

      if (_thread.get_id() == std::this_thread::get_id())
      {
        _thread.detach();
      }
      else
      {
        _thread.join();
      }
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
    using SignalAction = struct sigaction;

    static bool install(int const signal, SignalAction& oldAction)
    {
      auto action = SignalAction{};
      action.sa_handler = exitSignalHandler;
      ::sigemptyset(&action.sa_mask);
      action.sa_flags = 0;
      return ::sigaction(signal, &action, &oldAction) == 0;
    }

    static void restore(int const signal, SignalAction const& oldAction, bool const installed)
    {
      if (installed)
      {
        std::ignore = ::sigaction(signal, &oldAction, nullptr);
      }
    }

    std::shared_ptr<State> _statePtr;
    SignalAction _oldTerm{};
    SignalAction _oldHup{};
    bool _termInstalled = false;
    bool _hupInstalled = false;
    bool _ownsSignalHandlers = false;
    std::thread _thread{};
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
