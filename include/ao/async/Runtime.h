// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "Task.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>

#include <cstddef>
#include <exception>
#include <functional>
#include <future>

namespace ao::async
{
  class IExecutor;
  class LifetimeScope;

  class Runtime final
  {
  public:
    explicit Runtime(IExecutor& callbackExecutor);
    Runtime(IExecutor& callbackExecutor, std::size_t workerCount);
    ~Runtime();

    Runtime(Runtime const&) = delete;
    Runtime& operator=(Runtime const&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    IExecutor& callbackExecutor() noexcept;

    void requestStop() noexcept;
    void join();

    boost::asio::thread_pool& workerPool() noexcept;

    Task<void> resumeOnCallbackExecutor();
    Task<void> resumeOnWorker();

    void spawnLogged(Task<void> task);
    void spawn(Task<void> task, CancellationSlot slot, std::function<void(std::exception_ptr)> callback);

    template<typename T>
    std::future<T> spawn(Task<T> task)
    {
      return boost::asio::co_spawn(workerPool(), std::move(task), boost::asio::use_future);
    }

    void spawnWithLifetime(LifetimeScope* scope, Task<void> task);

  private:
    IExecutor& _callbackExecutor;
    boost::asio::thread_pool _workerPool;
  };
} // namespace ao::async
