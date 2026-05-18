// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "LifetimeScope.h"
#include "Runtime.h"
#include <ao/utility/Log.h>
// NOLINTBEGIN(misc-include-cleaner)
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
// NOLINTEND(misc-include-cleaner)
#include <exception>
#include <memory>
#include <mutex>
#include <utility>

// NOLINTBEGIN(misc-include-cleaner)
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
      sig->emit(boost::asio::cancellation_type::all);
    }
  }

  std::shared_ptr<LifetimeScopeState> LifetimeScope::state() const noexcept
  {
    return _state;
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void spawnWithLifetime(Runtime& runtime, LifetimeScope& scope, Task<void> task)
  {
    auto sig = std::make_shared<boost::asio::cancellation_signal>();
    auto state = scope.state();

    {
      auto lock = std::scoped_lock{state->mutex};

      if (!state->isAlive)
      {
        return;
      }

      state->signals.push_back(sig);
    }

    boost::asio::co_spawn(runtime.workerPool(),
                          std::move(task),
                          boost::asio::bind_cancellation_slot(
                            sig->slot(),
                            // NOLINTNEXTLINE(readability-identifier-length)
                            [state, sig](std::exception_ptr exPtr)
                            {
                              {
                                auto lock = std::scoped_lock{state->mutex};

                                if (state->isAlive)
                                {
                                  std::erase(state->signals, sig);
                                }
                              }

                              if (exPtr)
                              {
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
                            }));
  }
  // NOLINTEND(misc-include-cleaner)
} // namespace ao::rt::async
