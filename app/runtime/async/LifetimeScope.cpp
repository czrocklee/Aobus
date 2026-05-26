// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/async/LifetimeScope.h>
#include <ao/rt/async/Runtime.h>
#include <ao/rt/async/Task.h>
#include <ao/utility/Log.h>

#include <boost/asio/error.hpp>
#include <boost/system/system_error.hpp>

#include <exception>
#include <memory>
#include <mutex>
#include <utility>

namespace ao::rt::async
{
  LifetimeScope::LifetimeScope()
    : _state{std::make_shared<LifetimeScopeState>()}
  {
  }

  LifetimeScope::~LifetimeScope()
  {
    cancelAll();
  }

  void LifetimeScope::cancelAll()
  {
    auto lock = std::scoped_lock{_state->mutex};
    _state->isAlive = false;

    for (auto const& sig : _state->signals)
    {
      sig->emit(CancellationType::all);
    }
  }

  std::shared_ptr<LifetimeScopeState> LifetimeScope::state() const noexcept
  {
    return _state;
  }

  namespace
  {
    void handleCoroutineCompletion(std::shared_ptr<LifetimeScopeState> state,
                                   std::shared_ptr<CancellationSignal> sig,
                                   std::exception_ptr exPtr)
    {
      {
        auto lock = std::scoped_lock{state->mutex};

        if (state->isAlive)
        {
          std::erase(state->signals, sig);
        }
      }

      if (!exPtr)
      {
        return;
      }

      try
      {
        std::rethrow_exception(exPtr);
      }
      catch (boost::system::system_error const& se)
      {
        if (se.code() != boost::asio::error::operation_aborted)
        {
          APP_LOG_ERROR("Unhandled system error in lifetime-bound coroutine: {}", se.what());
        }
      }
      catch (std::exception const& ex)
      {
        APP_LOG_ERROR("Unhandled exception in lifetime-bound coroutine: {}", ex.what());
      }
      catch (...)
      {
        APP_LOG_ERROR("Unhandled unknown exception in lifetime-bound coroutine");
      }
    }
  }

  void Runtime::spawnWithLifetime(LifetimeScope* scope, Task<void> task)
  {
    auto sig = std::make_shared<CancellationSignal>();
    auto state = scope->state();

    {
      auto lock = std::scoped_lock{state->mutex};

      if (!state->isAlive)
      {
        return;
      }

      state->signals.push_back(sig);
    }

    spawn(std::move(task), sig->slot(), [state, sig](auto exPtr) { handleCoroutineCompletion(state, sig, exPtr); });
  }
} // namespace ao::rt::async
