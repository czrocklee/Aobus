// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Task.h"
#include <ao/utility/ScopedRegistration.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>

namespace ao::async
{
  class Executor;
  class LifetimeScope;
  class RuntimeTestAccess;

  using TaskHandle = utility::ScopedRegistration;

  class Runtime final
  {
  public:
    explicit Runtime(Executor& callbackExecutor);
    Runtime(Executor& callbackExecutor, std::size_t workerCount);
    ~Runtime();

    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    Executor& callbackExecutor() noexcept;

    void requestStop() noexcept;
    void join();

    boost::asio::thread_pool& workerPool() noexcept;

    Task<void> resumeOnCallbackExecutor();
    Task<void> resumeOnWorker();
    Task<void> sleepFor(std::chrono::milliseconds delay);

    void spawnLogged(Task<void> task);
    void spawn(Task<void> task, CancellationSlot slot, std::function<void(std::exception_ptr)> callback);
    TaskHandle spawnCancellable(Task<void> task);

    template<typename T>
    std::future<T> spawn(Task<T> task)
    {
      return boost::asio::co_spawn(workerPool(), std::move(task), boost::asio::use_future);
    }

    void spawnWithLifetime(LifetimeScope* scope, Task<void> task);

  private:
    friend class RuntimeTestAccess;

    Executor& _callbackExecutor;
    boost::asio::thread_pool _workerPool;
    std::function<Task<void>(std::chrono::milliseconds)> _sleepForOverride;
  };
} // namespace ao::async
