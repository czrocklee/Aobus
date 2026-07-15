// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Task.h"
#include <ao/async/AsyncExceptionHandler.h>
#include <ao/utility/ScopedRegistration.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <stop_token>
#include <string_view>

namespace ao::async
{
  class Executor;
  class LifetimeScope;
  class Sleeper;

  using TaskHandle = utility::ScopedRegistration;

  class Runtime final
  {
  public:
    // A non-null sleeper replaces the default steady-timer sleepFor with an
    // injected delay strategy; the Sleeper must outlive this Runtime.
    explicit Runtime(Executor& callbackExecutor,
                     AsyncExceptionHandler exceptionHandler = {},
                     Sleeper* sleeper = nullptr);
    Runtime(Executor& callbackExecutor,
            std::size_t workerCount,
            AsyncExceptionHandler exceptionHandler = {},
            Sleeper* sleeper = nullptr);
    ~Runtime();

    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    Executor& callbackExecutor() noexcept;

    // Consumes an exception at an application-owned async boundary. Expected
    // cancellation is silent; all other exceptions go to the injected handler
    // or the default stderr fallback.
    void reportUnhandledException(std::exception_ptr exceptionPtr, std::string_view context) const noexcept;

    void requestStop() noexcept;
    void join();

    boost::asio::thread_pool& workerPool() noexcept;

    Task<void> resumeOnCallbackExecutor(std::stop_token stopToken = {});
    Task<void> resumeOnWorker(std::stop_token stopToken = {});
    Task<void> sleepFor(std::chrono::milliseconds delay, std::stop_token stopToken = {});

    void spawnLogged(Task<void> task);
    TaskHandle spawnCancellable(CancellableTask task);

    template<typename T>
    std::future<T> spawn(Task<T> task)
    {
      return boost::asio::co_spawn(workerPool(), std::move(task), boost::asio::use_future);
    }

    void spawnWithLifetime(LifetimeScope* scope, CancellableTask task);

  private:
    void handleUnhandledException(std::exception_ptr exceptionPtr, std::string_view context) const noexcept;

    std::move_only_function<void()> startCancellable(CancellableTask task,
                                                     std::function<void(std::exception_ptr)> completion);

    Executor& _callbackExecutor;
    // Terminal completion closures borrow this Runtime. The destructor stops
    // and joins _workerPool before member teardown.
    AsyncExceptionHandler _exceptionHandler;
    boost::asio::thread_pool _workerPool;
    Sleeper* _sleeper;
  };
} // namespace ao::async
